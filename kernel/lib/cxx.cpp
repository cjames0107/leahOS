// Minimal C++ runtime support for a freestanding kernel.
//
// We compile with -fno-exceptions and -fno-rtti, but the compiler still emits
// references to a handful of ABI symbols. Supplying them here means we never
// have to link against libsupc++.

#include <leah/types.hpp>

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
