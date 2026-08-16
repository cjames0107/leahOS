/* The stack canary, and what to do when it is gone.
 *
 * Built with -fstack-protector-strong, the compiler puts a known value between
 * a function's local arrays and its saved return address, and checks it before
 * returning. An overrun that walks off the end of a local buffer trips it at
 * the point of the overrun rather than at whatever the corruption later makes
 * fail - which for the startup fault was a `push` in a completely unrelated
 * function, two calls and one exec later.
 *
 * The guard is a fixed value rather than a random one. There is no entropy at
 * this point in startup, and this is here to find a bug rather than to stop an
 * attacker: a predictable canary catches the accident just as well.
 */

#include <stdlib.h>
#include <unistd.h>

unsigned long __stack_chk_guard = 0x00FA57C0DEBEEF00ul;

__attribute__((noreturn)) void __stack_chk_fail(void)
{
    /* Straight to the console rather than through stdio's buffering: the stack
     * this is reporting about is already damaged, and anything that returns
     * through it may not arrive. */
    static const char kMessage[] =
        "\n*** stack smashed: a local buffer was written past its end ***\n";
    write(2, kMessage, sizeof(kMessage) - 1);
    exit(134);
    for (;;) { }        /* exit does not return; this is for the compiler */
}
