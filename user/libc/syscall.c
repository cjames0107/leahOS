#include <sys/syscall.h>

/* The one place the SYSCALL instruction is issued from user space.
 *
 * The kernel takes the call number in RAX and arguments in RDI, RSI, RDX, R10,
 * R8 - the SysV order, with R10 standing in for the RCX that SYSCALL destroys.
 * SYSCALL also clobbers R11 (it saves RFLAGS there), so both are told to the
 * compiler as clobbers. "memory" keeps the compiler from reordering loads and
 * stores across a call that can have arbitrary side effects. */
long __syscall(long number, long a1, long a2, long a3, long a4, long a5)
{
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;

    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return result;
}
