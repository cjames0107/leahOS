/* audiod - the sound card's driver, in ring 3.
 *
 * The same AC'97 the kernel used to drive, moved out and given the same three
 * grants every other driver here gets: the PCI configuration ports, the card's
 * own two port ranges, and memory the card can be pointed at. Nothing else.
 *
 * The hard-won parts came with it and are worth keeping stated, because each
 * one was silence rather than an error when it was wrong:
 *
 *  - The descriptor list is prefilled so every entry points at a real buffer.
 *    An engine that gets ahead of the software then plays silence out of memory
 *    this driver owns rather than DMAing from physical address zero.
 *  - How much is still queued is asked of the card, never counted here. A
 *    private tally kept in step with a card that advances on its own is wrong
 *    for a long time before anyone notices.
 *  - A halted engine starts the list again from the top rather than having its
 *    last-valid index nudged along, because where it resumes from otherwise is
 *    not something two cards will agree about.
 *  - The codec is told the sample rate. Without it the controller accepts every
 *    buffer, advances through the list at memory speed, and plays none of it.
 */

#include <aud.h>
#include <driver.h>
#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

/* The mixer, on the first I/O BAR. */
#define NAM_RESET     0x00
#define NAM_MASTER    0x02
#define NAM_PCM       0x18
#define NAM_POWERDOWN 0x26
#define NAM_EXT_ID    0x28
#define NAM_EXT_CTRL  0x2A
#define NAM_DAC_RATE  0x2C
#define EXT_VRA       0x0001

/* The bus master's PCM-out box, on the second. */
#define PO_BDBAR 0x10
#define PO_CIV   0x14
#define PO_LVI   0x15
#define PO_SR    0x16
#define PO_CR    0x1B
#define GLOB_CNT 0x2C

#define SR_DCH   0x01
#define CR_RPBM  0x01
#define CR_RR    0x02

#define BUFFERS 32
#define BUF_SAMPLES 2048
#define BUF_BYTES (BUF_SAMPLES * 2)

struct descriptor {
    uint32_t address;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed));

static unsigned g_nam, g_nabm;
static struct descriptor* g_list;
static uint64_t g_list_phys, g_audio_phys;
static int16_t* g_audio;
static unsigned g_head, g_fill;
static int g_volume = 80;
static char g_name[32] = "AC'97";
static struct aud_shared* g_ring;

static unsigned cfg_read(unsigned bus, unsigned slot, unsigned fn, unsigned off)
{
    outl(PCI_ADDRESS, 0x80000000u | (bus << 16) | (slot << 11) | (fn << 8) |
                      (off & 0xFC));
    return inl(PCI_DATA);
}

static void cfg_write(unsigned bus, unsigned slot, unsigned fn, unsigned off,
                      unsigned value)
{
    outl(PCI_ADDRESS, 0x80000000u | (bus << 16) | (slot << 11) | (fn << 8) |
                      (off & 0xFC));
    outl(PCI_DATA, value);
}

/* 0 is loudest here: the mixer takes attenuation in 1.5 dB steps, and bit 15
 * mutes outright - the only way to get true silence, because even maximum
 * attenuation is faintly audible. */
static unsigned attenuation(int percent, unsigned steps)
{
    if (percent <= 0)
        return 0x8000;
    if (percent > 100)
        percent = 100;
    const unsigned level = (unsigned)(100 - percent) * steps / 100;
    return (level << 8) | level;
}

static void write_volume(void)
{
    outw(g_nam + NAM_MASTER, (unsigned short)attenuation(g_volume, 63));
    outw(g_nam + NAM_PCM, (unsigned short)attenuation(100, 31));
}

static unsigned in_flight(void)
{
    if (inb(g_nabm + PO_SR) & SR_DCH)
        return 0;
    const unsigned civ = inb(g_nabm + PO_CIV);
    const unsigned lvi = inb(g_nabm + PO_LVI);
    return (lvi + BUFFERS - civ) % BUFFERS + 1;
}

static unsigned free_buffers(void)
{
    const unsigned busy = in_flight();
    return busy + 1 >= BUFFERS ? 0 : BUFFERS - 1 - busy;
}

static void reset_box(void)
{
    outb(g_nabm + PO_CR, 0);
    outb(g_nabm + PO_CR, CR_RR);
    for (unsigned i = 0; i < 100000 && (inb(g_nabm + PO_CR) & CR_RR); ++i)
        ;
    outb(g_nabm + PO_SR, inb(g_nabm + PO_SR));
    outl(g_nabm + PO_BDBAR, (unsigned)g_list_phys);
    outb(g_nabm + PO_LVI, 0);
}

/* How many buffers to have ready before letting the card start. The card stops
 * the moment it plays the buffer that LVI points at, so starting on a single
 * buffer means halting on that same buffer: the writer then has one buffer's
 * worth of time to produce the next one or the sound breaks. Handing it a few
 * up front buys the writer that much slack, at the cost of the same amount of
 * latency - four buffers is 85ms, which no one hears at the start of a sound
 * and which is far more scheduling room than a writer needs. */
#define PREFILL 4

static int      g_running;      /* the engine is going and has not underrun */
static unsigned g_queued;       /* buffers handed over since it last stopped */

/* Start it now, whatever is ready. A sound shorter than the prefill would
 * otherwise sit in the buffers and never play. */
static void force_start(void)
{
    if (!g_running && g_queued > 0) {
        outb(g_nabm + PO_CR, CR_RPBM);
        g_running = 1;
    }
}

static void submit(unsigned samples)
{
    /* DCH means halted, which before the first start is simply the truth and
     * not a fault - so only treat it as an underrun once we have started. */
    if (g_running && (inb(g_nabm + PO_SR) & SR_DCH)) {
        reset_box();
        if (g_head != 0) {
            memcpy(&g_audio[0], &g_audio[g_head * BUF_SAMPLES], samples * 2);
            g_head = 0;
        }
        g_running = 0;
        g_queued  = 0;
    }
    g_list[g_head].samples = (unsigned short)samples;
    /* LVI can move while the engine is stopped; it is only an index. The run
     * bit goes on once there is a queue behind it. */
    outb(g_nabm + PO_LVI, (unsigned char)g_head);
    g_head = (g_head + 1) % BUFFERS;
    g_fill = 0;
    if (!g_running && ++g_queued >= PREFILL) {
        outb(g_nabm + PO_CR, CR_RPBM);
        g_running = 1;
    }
}

/* Move whatever the writer has left in the ring into the card's buffers. */
static long pump(void)
{
    long taken = 0;
    for (;;) {
        const unsigned head = g_ring->head;
        const unsigned tail = g_ring->tail;
        if (head == tail)
            break;
        if (g_fill == 0 && free_buffers() == 0)
            break;
        unsigned available = (head + AUD_RING - tail) % AUD_RING;
        unsigned room = BUF_SAMPLES - g_fill;
        if (available > room)
            available = room;
        /* The ring wraps; one pass takes only what is contiguous. */
        if (tail + available > AUD_RING)
            available = AUD_RING - tail;
        memcpy(&g_audio[g_head * BUF_SAMPLES + g_fill],
               (const void*)&g_ring->samples[tail], available * 2);
        g_fill += available;
        g_ring->tail = (tail + available) % AUD_RING;
        taken += available;
        if (g_fill == BUF_SAMPLES)
            submit(BUF_SAMPLES);
    }
    return taken;
}

static int bring_up(void)
{
    if (io_permit(PCI_ADDRESS, 8) != 0)
        return -1;

    unsigned bus = 0, slot = 0;
    int found = 0;
    for (bus = 0; bus < 4 && !found; ++bus)
        for (slot = 0; slot < 32; ++slot) {
            const unsigned id = cfg_read(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            const unsigned cls = cfg_read(bus, slot, 0, 0x08) >> 16;
            /* Class 4 subclass 1: a multimedia audio controller, which in
             * practice means AC'97. */
            if ((cls & 0xFFFF) == 0x0401) { found = 1; break; }
        }
    if (!found)
        return -1;
    --bus;

    const unsigned command = cfg_read(bus, slot, 0, 0x04);
    cfg_write(bus, slot, 0, 0x04, command | (1u << 0) | (1u << 2));

    g_nam  = cfg_read(bus, slot, 0, 0x10) & ~3u;
    g_nabm = cfg_read(bus, slot, 0, 0x14) & ~3u;
    if (g_nam == 0 || g_nabm == 0)
        return -1;
    if (io_permit(g_nam, 64) != 0 || io_permit(g_nabm, 64) != 0)
        return -1;

    outl(g_nabm + GLOB_CNT, 1u << 1);           /* out of cold reset */
    outw(g_nam + NAM_RESET, 0);
    for (unsigned i = 0; i < 1000000; ++i)
        if ((inw(g_nam + NAM_POWERDOWN) & 0x0F) == 0x0F)
            break;

    /* Say the rate out loud, or the card accepts everything and plays none. */
    if (inw(g_nam + NAM_EXT_ID) & EXT_VRA) {
        outw(g_nam + NAM_EXT_CTRL,
             (unsigned short)(inw(g_nam + NAM_EXT_CTRL) | EXT_VRA));
        outw(g_nam + NAM_DAC_RATE, 48000);
    }

    g_list  = (struct descriptor*)dma_alloc(BUFFERS * sizeof(struct descriptor),
                                            &g_list_phys);
    g_audio = (int16_t*)dma_alloc(BUFFERS * BUF_BYTES, &g_audio_phys);
    if (g_list == 0 || g_audio == 0)
        return -1;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        g_list[i].address = (unsigned)(g_audio_phys + (uint64_t)i * BUF_BYTES);
        g_list[i].samples = BUF_SAMPLES;
        g_list[i].flags = 0;
    }
    reset_box();
    write_volume();
    return 0;
}

int main(void)
{
    if (bring_up() != 0) {
        printf("audiod: no AC'97 controller\n");
        return 1;
    }

    const int shm = shm_open(AUD_SHM_KEY, sizeof(struct aud_shared), SHM_PUBLIC);
    g_ring = shm < 0 ? 0 : (struct aud_shared*)shm_map(shm);
    if (g_ring == 0) {
        printf("audiod: cannot publish the sample ring\n");
        return 1;
    }
    memset((void*)g_ring, 0, sizeof(*g_ring));

    const int port = port_create(IPC_PORT_AUDIO);
    if (port < 0) {
        printf("audiod: the card already has a driver\n");
        return 1;
    }
    printf("audiod[%d]: %s, 48000 Hz stereo, in ring 3\n", getpid(), g_name);

    for (;;) {
        pump();

        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_try_recv(port, &m, &from);
        if (handle < 0) {
            msleep(2);
            continue;
        }
        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        if (m.tag == AUD_INFO) {
            r.word[0] = 48000;
            r.word[1] = 2;
            unsigned n = 0;
            while (g_name[n] != '\0' && n < sizeof(r.data) - 1) {
                r.data[n] = g_name[n];
                ++n;
            }
            r.bytes = n;
        } else if (m.tag == AUD_KICK) {
            r.word[0] = pump();
        } else if (m.tag == AUD_SPACE) {
            const unsigned used =
                (g_ring->head + AUD_RING - g_ring->tail) % AUD_RING;
            r.word[0] = (long)(AUD_RING - 1 - used);
        } else if (m.tag == AUD_FLUSH) {
            pump();
            if (g_fill > 0 && free_buffers() > 0)
                submit(g_fill);
            force_start();
            r.word[0] = 0;
        } else if (m.tag == AUD_STOP) {
            reset_box();
            memset(g_audio, 0, BUFFERS * BUF_BYTES);
            g_head = g_fill = 0;
            g_running = 0;
            g_queued = 0;
            g_ring->tail = g_ring->head;
            r.word[0] = 0;
        } else if (m.tag == AUD_VOLUME) {
            if (m.word[0] >= 0) {
                g_volume = (int)m.word[0];
                write_volume();
            }
            r.word[0] = g_volume;
        } else {
            r.word[0] = -1;
        }
        ipc_reply(handle, &r);
    }
}
