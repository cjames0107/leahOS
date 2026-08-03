#include <leah/fpu.hpp>
#include <leah/string.hpp>

namespace fpu {
namespace {

// CR0
constexpr u64 kMonitorCoprocessor = 1ull << 1;   // MP
constexpr u64 kEmulation          = 1ull << 2;   // EM - set means "no FPU here"
constexpr u64 kTaskSwitched       = 1ull << 3;   // TS - would trap on first use
constexpr u64 kNumericError       = 1ull << 5;   // NE - report x87 faults as #MF

// CR4
constexpr u64 kOsFxsr     = 1ull << 9;           // FXSAVE/FXRSTOR are usable
constexpr u64 kOsXmmExcpt = 1ull << 10;          // SIMD faults arrive as #XM

// Offsets into the FXSAVE image, from the manual's layout.
constexpr usize kFcwOffset        = 0;
constexpr usize kMxcsrOffset      = 24;
constexpr usize kMxcsrMaskOffset  = 28;

constexpr u16 kFcwDefault   = 0x037F;  // all x87 exceptions masked, 64-bit
constexpr u32 kMxcsrDefault = 0x1F80;  // all SIMD exceptions masked
// Which MXCSR bits this CPU actually implements. FXSAVE reports it, and
// FXRSTOR faults on any set bit outside it - so a restore is only ever given
// bits the CPU agreed to. 0xFFBF is the value to assume when a CPU reports
// zero, which the manual says means exactly that.
constexpr u32 kMxcsrMaskFallback = 0x0000FFBF;

bool g_available;
u32  g_mxcsr_mask = kMxcsrMaskFallback;

struct CpuidResult { u32 eax, ebx, ecx, edx; };

CpuidResult cpuid(u32 leaf)
{
    CpuidResult r{};
    asm volatile("cpuid"
                 : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                 : "a"(leaf), "c"(0));
    return r;
}

u64 read_cr0() { u64 v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
void write_cr0(u64 v) { asm volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
u64 read_cr4() { u64 v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
void write_cr4(u64 v) { asm volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

}  // namespace

bool init_this_cpu()
{
    constexpr u32 kFxsrBit = 1u << 24;
    constexpr u32 kSseBit  = 1u << 25;
    constexpr u32 kSse2Bit = 1u << 26;

    const CpuidResult features = cpuid(1);
    if ((features.edx & (kFxsrBit | kSseBit | kSse2Bit)) !=
        (kFxsrBit | kSseBit | kSse2Bit))
        return false;

    // EM off and MP on: SSE instructions execute rather than faulting, and
    // WAIT respects TS. TS stays off because this kernel saves eagerly at the
    // switch - there is no lazy scheme for a #NM fault to drive.
    u64 cr0 = read_cr0();
    cr0 &= ~(kEmulation | kTaskSwitched);
    cr0 |= kMonitorCoprocessor | kNumericError;
    write_cr0(cr0);

    write_cr4(read_cr4() | kOsFxsr | kOsXmmExcpt);

    // A known state on this CPU before anything is saved from it.
    asm volatile("fninit");
    const u32 mxcsr = kMxcsrDefault;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));

    // Ask this CPU which MXCSR bits it implements, once, from a real save.
    State probe;
    memset(probe.bytes, 0, sizeof(probe.bytes));
    asm volatile("fxsave %0" : "=m"(probe) : : "memory");
    u32 reported;
    memcpy(&reported, probe.bytes + kMxcsrMaskOffset, sizeof(reported));
    g_mxcsr_mask = (reported != 0) ? reported : kMxcsrMaskFallback;

    g_available = true;
    return true;
}

bool available() { return g_available; }

void init_state(State& state)
{
    memset(state.bytes, 0, sizeof(state.bytes));

    const u16 fcw = kFcwDefault;
    memcpy(state.bytes + kFcwOffset, &fcw, sizeof(fcw));
    const u32 mxcsr = kMxcsrDefault;
    memcpy(state.bytes + kMxcsrOffset, &mxcsr, sizeof(mxcsr));
    memcpy(state.bytes + kMxcsrMaskOffset, &g_mxcsr_mask, sizeof(g_mxcsr_mask));
}

void save(State& state)
{
    if (!g_available)
        return;
    asm volatile("fxsave %0" : "=m"(state) : : "memory");
}

void restore(const State& state)
{
    if (!g_available)
        return;
    asm volatile("fxrstor %0" : : "m"(state) : "memory");
}

void restore_untrusted(const State& state)
{
    if (!g_available)
        return;
    // A copy, because the caller's is in user memory and must not be written,
    // and because FXRSTOR reads MXCSR from it: a bit set outside what this CPU
    // implements is a #GP taken inside the kernel on a value a process chose.
    State safe = state;
    u32 mxcsr;
    memcpy(&mxcsr, safe.bytes + kMxcsrOffset, sizeof(mxcsr));
    mxcsr &= g_mxcsr_mask;
    memcpy(safe.bytes + kMxcsrOffset, &mxcsr, sizeof(mxcsr));

    asm volatile("fxrstor %0" : : "m"(safe) : "memory");
}

}  // namespace fpu
