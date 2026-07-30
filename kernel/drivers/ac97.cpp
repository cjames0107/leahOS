#include <leah/ac97.hpp>
#include <leah/console.hpp>
#include <leah/io.hpp>
#include <leah/memory.hpp>
#include <leah/pci.hpp>
#include <leah/pmm.hpp>
#include <leah/spinlock.hpp>
#include <leah/string.hpp>

namespace audio {
namespace {

// --- the mixer, on the first I/O BAR ----------------------------------------
constexpr u16 kNamReset      = 0x00;
constexpr u16 kNamMasterVol  = 0x02;
constexpr u16 kNamPcmVol     = 0x18;
constexpr u16 kNamPowerdown  = 0x26;
constexpr u16 kNamExtId      = 0x28;   // what the codec can do
constexpr u16 kNamExtCtrl    = 0x2A;   // what it is asked to do
constexpr u16 kNamDacRate    = 0x2C;   // front DAC sample rate, when VRA is on

constexpr u16 kExtVra = 1 << 0;        // variable rate audio

// --- the bus master, on the second --------------------------------------
// The PCM-out box. There are three identical boxes (in, out, mic); this is the
// only one that matters until something wants to record.
constexpr u16 kPoBdbar = 0x10;   // 32-bit physical address of the descriptor list
constexpr u16 kPoCiv   = 0x14;   // which descriptor is playing
constexpr u16 kPoLvi   = 0x15;   // the last one with anything in it
constexpr u16 kPoSr    = 0x16;   // status
constexpr u16 kPoPicb  = 0x18;   // samples left in the current buffer
constexpr u16 kPoCr    = 0x1B;   // control

constexpr u16 kGlobalCnt = 0x2C;

constexpr u8 kSrDch = 1 << 0;    // DMA halted: it ran out of descriptors
constexpr u8 kCrRpbm = 1 << 0;   // run
constexpr u8 kCrRr   = 1 << 1;   // reset this box

// A descriptor is an address, a sample count, and two flag bits. The card walks
// the list by itself; the only thing software does per buffer is say how far
// along the list is valid.
struct __attribute__((packed)) Descriptor {
    u32 address;
    u16 samples;                 // 16-bit samples, not bytes and not frames
    u16 flags;
};

// 32 is the hardware maximum, and using all of it costs 128 KiB and buys about
// a third of a second of slack - enough that a program which is slow to come
// back with more audio is not immediately audible as a gap.
constexpr usize kBuffers      = 32;
constexpr usize kBufferSamples = 2048;          // ~21 ms of stereo at 48 kHz
constexpr usize kBufferBytes  = kBufferSamples * sizeof(i16);

u16 g_nam;                       // mixer ports
u16 g_nabm;                      // bus master ports
bool g_ready;
const char* g_name = "none";

Descriptor* g_list;
paddr_t     g_list_phys;
i16*        g_audio;             // kBuffers * kBufferSamples, contiguous
paddr_t     g_audio_phys;

// Which buffer is being filled, and how far into it. Only whole buffers are
// handed over: a descriptor per write would mean a program writing 256 samples
// at a time exhausted the list of 32 after 8 KiB, which is a sixth of a second
// and then silence. The tail goes out on an explicit flush.
//
// How many buffers are still to be played is *not* tracked here. It was, and
// keeping a private count in step with a card that advances on its own is the
// kind of bookkeeping that is subtly wrong for a long time - it read as "two
// queued" while the card had actually stopped, and all but one buffer was
// overwritten before it could be played. The card already knows; ask it.
usize g_head;                    // buffer index being filled
usize g_fill;                    // samples already in it
u32   g_volume = 80;

// Everything here is a read-modify-write over shared ports and a shared ring,
// and play() is called from a syscall on any processor.
sync::Spinlock g_lock;

void* alloc_dma(usize bytes, paddr_t& phys_out)
{
    const usize pages = (bytes + pmm::kPageSize - 1) / pmm::kPageSize;
    const paddr_t phys = pmm::alloc_contiguous(pages);
    if (phys == 0)
        return nullptr;
    phys_out = phys;
    void* virt = reinterpret_cast<void*>(memory::phys_to_direct(phys));
    memset(virt, 0, pages * pmm::kPageSize);
    return virt;
}

// The mixer wants attenuation, not volume: 0 is loudest and each step is 1.5 dB
// quieter. Bit 15 mutes outright, which is the only way to get true silence -
// maximum attenuation is still faintly audible.
u16 attenuation(u32 percent, u32 steps)
{
    if (percent == 0)
        return 0x8000;
    if (percent > 100)
        percent = 100;
    const u32 level = (100 - percent) * steps / 100;
    return static_cast<u16>((level << 8) | level);
}

void write_volume()
{
    if (!g_ready)
        return;
    io::out16(g_nam + kNamMasterVol, attenuation(g_volume, 63));
    // PCM playback stays wide open; the master control is the one users mean.
    io::out16(g_nam + kNamPcmVol, attenuation(100, 31));
}

// Buffers handed over and not yet finished, straight from the card. CIV is the
// descriptor being played and LVI the last one with anything in it, so the
// span between them - inclusive, because the one playing is not done - is what
// is still owed. A halted engine owes nothing.
usize in_flight()
{
    if ((io::in8(g_nabm + kPoSr) & kSrDch) != 0)
        return 0;
    const u8 civ = io::in8(g_nabm + kPoCiv);
    const u8 lvi = io::in8(g_nabm + kPoLvi);
    return (static_cast<usize>(lvi) + kBuffers - civ) % kBuffers + 1;
}

// One descriptor is always held back, so LVI can never land on CIV: the card
// would read that as a full list rather than an empty one.
usize free_buffers()
{
    const usize busy = in_flight();
    return busy + 1 >= kBuffers ? 0 : kBuffers - 1 - busy;
}

// Hand the buffer at g_head to the card and move on.
// Put the box back to a known state: stopped, at descriptor 0, with the list
// address reloaded.
void reset_box()
{
    io::out8(g_nabm + kPoCr, 0);
    io::out8(g_nabm + kPoCr, kCrRr);
    for (u32 i = 0; i < 100000 && (io::in8(g_nabm + kPoCr) & kCrRr) != 0; ++i)
        ;
    io::out8(g_nabm + kPoSr, io::in8(g_nabm + kPoSr));   // acknowledge everything
    io::out32(g_nabm + kPoBdbar, static_cast<u32>(g_list_phys));
    io::out8(g_nabm + kPoLvi, 0);
}

void submit(usize samples)
{
    // A halted engine is one that reached LVI and stopped. Where it resumes
    // from if LVI is simply moved on is not something the specification is
    // clear about and not something this card and the next will agree on - and
    // getting it wrong is silent, literally: the first buffer played and every
    // one after it came out as silence, because the engine had wandered into
    // descriptors that had never been filled.
    //
    // So a halt starts the list again from the top. Nothing is lost by it:
    // halted means everything handed over has already been played.
    const bool halted = (io::in8(g_nabm + kPoSr) & kSrDch) != 0;
    if (halted) {
        reset_box();
        if (g_head != 0) {
            memcpy(&g_audio[0], &g_audio[g_head * kBufferSamples],
                   samples * sizeof(i16));
            g_head = 0;
        }
    }

    g_list[g_head].samples = static_cast<u16>(samples);

    // Order matters, and not symmetrically. Moving LVI is what wakes a halted
    // engine, but only if it is already running - so on a cold start the run
    // bit has to go first, and on a warm one LVI has to. Doing both, in this
    // order, is correct either way: the run bit is a no-op when already set.
    io::out8(g_nabm + kPoCr, kCrRpbm);
    io::out8(g_nabm + kPoLvi, static_cast<u8>(g_head));

    g_head = (g_head + 1) % kBuffers;
    g_fill = 0;
}

} // namespace

bool available() { return g_ready; }
const char* device_name() { return g_name; }

u32 volume() { return g_volume; }

void set_volume(u32 percent)
{
    sync::IrqScopedLock guard(g_lock);
    g_volume = percent > 100 ? 100 : percent;
    write_volume();
}

usize space()
{
    if (!g_ready)
        return 0;
    sync::IrqScopedLock guard(g_lock);
    return free_buffers() * kBufferSamples + (kBufferSamples - g_fill);
}

usize play(const i16* samples, usize count)
{
    if (!g_ready || samples == nullptr)
        return 0;
    sync::IrqScopedLock guard(g_lock);

    usize taken = 0;
    while (taken < count) {
        // A buffer only needs a free descriptor once it is full, so a
        // part-filled one can always be topped up.
        if (g_fill == 0 && free_buffers() == 0)
            break;
        const usize room = kBufferSamples - g_fill;
        usize chunk = count - taken;
        if (chunk > room)
            chunk = room;
        memcpy(&g_audio[g_head * kBufferSamples + g_fill], &samples[taken],
               chunk * sizeof(i16));
        g_fill += chunk;
        taken  += chunk;
        if (g_fill == kBufferSamples)
            submit(kBufferSamples);
    }
    return taken;
}

void flush()
{
    if (!g_ready)
        return;
    sync::IrqScopedLock guard(g_lock);
    if (g_fill > 0 && free_buffers() > 0)
        submit(g_fill);
}

void stop()
{
    if (!g_ready)
        return;
    sync::IrqScopedLock guard(g_lock);
    reset_box();
    memset(g_audio, 0, kBuffers * kBufferBytes);
    g_head = g_fill = 0;
}

Status status()
{
    Status out{};
    if (!g_ready)
        return out;
    out.current_index  = io::in8(g_nabm + kPoCiv);
    out.last_valid     = io::in8(g_nabm + kPoLvi);
    out.status_reg     = io::in8(g_nabm + kPoSr);
    out.control_reg    = io::in8(g_nabm + kPoCr);
    out.position       = io::in16(g_nabm + kPoPicb);
    out.queued_buffers = static_cast<u32>(in_flight());
    return out;
}

bool init()
{
    // Class 4 subclass 1 is "multimedia audio controller", which in practice
    // means AC'97. HD Audio is subclass 3 and would need a different driver.
    const pci::Device* dev = pci::find(0x04, 0x01);
    if (dev == nullptr)
        return false;

    // I/O space and bus mastering: the card reads the descriptor list and the
    // samples out of memory by itself.
    constexpr u8 kCommand = 0x04;
    u32 command = pci::read32(dev->bus, dev->slot, dev->function, kCommand);
    command |= (1 << 0) | (1 << 2);
    pci::write32(dev->bus, dev->slot, dev->function, kCommand, command);

    bool nam_io = false, nabm_io = false;
    const u64 bar0 = pci::bar_address(*dev, 0, nam_io);
    const u64 bar1 = pci::bar_address(*dev, 1, nabm_io);
    if (!nam_io || !nabm_io || bar0 == 0 || bar1 == 0)
        return false;
    g_nam  = static_cast<u16>(bar0);
    g_nabm = static_cast<u16>(bar1);

    // Cold reset, then wait for the codec to say it is ready. Writing anything
    // to the mixer's reset register is what wakes it.
    io::out32(g_nabm + kGlobalCnt, (1 << 1));
    for (u32 i = 0; i < 1000000; ++i) {
        if ((io::in32(g_nabm + kGlobalCnt) & (1 << 1)) != 0)
            break;
    }
    io::out16(g_nam + kNamReset, 0);

    // Wait for the DAC to come up. Bits 1..3 of the powerdown register read
    // back set when the analogue sections are ready; going on before that
    // leaves the codec configured and deaf.
    for (u32 i = 0; i < 1000000; ++i) {
        if ((io::in16(g_nam + kNamPowerdown) & 0x0F) == 0x0F)
            break;
    }

    // Say out loud what rate the samples are.
    //
    // Without this the codec is left at whatever rate it powered up believing,
    // and a controller that disagrees with the stream it is given does not
    // complain - it accepts every buffer, advances through the list at memory
    // speed, and plays none of it. That is exactly what happened: the
    // descriptor index kept perfect pace with the writer and one buffer in
    // eight reached the speakers.
    const u16 caps = io::in16(g_nam + kNamExtId);
    if ((caps & kExtVra) != 0) {
        io::out16(g_nam + kNamExtCtrl,
                  static_cast<u16>(io::in16(g_nam + kNamExtCtrl) | kExtVra));
        io::out16(g_nam + kNamDacRate, static_cast<u16>(kSampleRate));
    }

    g_list = static_cast<Descriptor*>(
        alloc_dma(kBuffers * sizeof(Descriptor), g_list_phys));
    g_audio = static_cast<i16*>(alloc_dma(kBuffers * kBufferBytes, g_audio_phys));
    if (g_list == nullptr || g_audio == nullptr)
        return false;

    // Everything the card DMAs from has to be addressable in the 32 bits a
    // descriptor has. With the memory this machine has that is always true, but
    // silently playing from the wrong address would be a horrible way to find
    // out otherwise.
    if ((g_audio_phys + kBuffers * kBufferBytes) > 0xFFFFFFFFull ||
        g_list_phys > 0xFFFFFFFFull)
        return false;

    // Every descriptor points at its own buffer from the start, and the
    // buffers are zeroed. An engine that gets ahead of the software then reads
    // silence out of memory this driver owns, rather than DMAing from whatever
    // physical address zero happens to be.
    for (usize i = 0; i < kBuffers; ++i) {
        g_list[i].address = static_cast<u32>(g_audio_phys + i * kBufferBytes);
        g_list[i].samples = static_cast<u16>(kBufferSamples);
        g_list[i].flags   = 0;
    }

    reset_box();

    g_ready = true;
    g_name = dev->vendor_id == 0x8086 ? "Intel 82801AA AC'97" : "AC'97 codec";
    write_volume();
    return true;
}

} // namespace audio
