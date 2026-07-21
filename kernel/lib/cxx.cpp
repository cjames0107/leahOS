// Minimal C++ runtime support for a freestanding kernel.
//
// We compile with -fno-exceptions and -fno-rtti, but the compiler still emits
// references to a handful of ABI symbols. Supplying them here means we never
// have to link against libsupc++.

#include <leah/types.hpp>

// Placed by the linker script. Nothing references these pointers by name, so
// the only way constructors ever run is by walking the array ourselves.
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

// Run constructors for objects with static storage duration.
//
// Worth knowing what this protects against: when GCC cannot constant-evaluate
// an initialiser - exceeding the constexpr loop limit is enough - it silently
// demotes the object to .bss plus a runtime constructor. With no one calling
// that constructor the object is simply zero forever, and nothing warns. Mark
// such objects constinit if they must be compile-time initialised.
//
// Called before the heap exists, so a constructor here must not allocate.
void run_global_constructors()
{
    for (auto** entry = __init_array_start; entry != __init_array_end; ++entry)
        (*entry)();
}

extern "C" {

// Emitted for a call through a pure-virtual slot, which means the object was
// used during or after its own construction/destruction.
void __cxa_pure_virtual()
{
    for (;;)
        asm volatile("cli; hlt");
}

// Registers destructors for objects with static storage duration. The kernel
// never exits, so there is nothing to run them at.
void* __dso_handle = nullptr;

int __cxa_atexit(void (*)(void*), void*, void*)
{
    return 0;
}

// Guards for function-local statics. Single-CPU and interrupts-off for now, so
// no locking is required; this needs revisiting when we bring up SMP.
int __cxa_guard_acquire(u64* guard)
{
    return *reinterpret_cast<u8*>(guard) == 0;
}

void __cxa_guard_release(u64* guard)
{
    *reinterpret_cast<u8*>(guard) = 1;
}

void __cxa_guard_abort(u64*)
{
}

} // extern "C"
