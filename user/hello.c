/* The first leahOS program written in C rather than assembly.
 *
 * Everything here goes through libc, which goes through the SYSCALL ABI: the
 * printf output reaches the screen via the write syscall, and returning from
 * main runs crt0's tail, which calls exit. The kernel checks the exit status,
 * so the return value is the machine-checkable half of the test and the
 * printed lines are the human-readable half. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("hello from a C program running in ring 3\n");
    printf("  pid          = %d\n", getpid());

    /* Exercise malloc and the string routines, so a wrong exit code can point
     * at the libc rather than only at the syscall path. */
    char* greeting = malloc(64);
    if (greeting == NULL)
        return 1;
    strcpy(greeting, "leahOS");

    printf("  malloc'd str = %s (len %d)\n", greeting, (int)strlen(greeting));
    printf("  hex/pointer  = 0x%x %p\n", 0x1EA4, (void*)greeting);

    /* A large allocation forces the heap to grow past its first sbrk chunk, and
     * writing every byte proves the new pages are really mapped. */
    const int big = 300 * 1024;
    unsigned char* buffer = malloc((unsigned long)big);
    if (buffer == NULL)
        return 1;
    for (int i = 0; i < big; ++i)
        buffer[i] = (unsigned char)(i * 7 + 1);
    int ok = 1;
    for (int i = 0; i < big; ++i)
        ok &= buffer[i] == (unsigned char)(i * 7 + 1);
    printf("  sbrk heap    = %d KiB allocated and verified: %s\n",
           big / 1024, ok ? "yes" : "NO");

    /* A value the kernel's self-test recognises, built so a wrong result tells
     * printf/malloc apart from the raw exit path. */
    return 0x42;
}
