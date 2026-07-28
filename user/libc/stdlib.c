#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

_Noreturn void exit(int status)
{
    __syscall(SYS_exit, status, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* A bump allocator over sbrk. The break grows a page at a time as demand
 * exceeds it, contiguously, so no space is wasted between growths. free is a
 * no-op - real reclamation is a job for a later allocator; this is enough for
 * programs that allocate a bounded amount. */
static char* g_top = 0;      /* next allocation */
static char* g_end = 0;      /* current program break */

void* malloc(size_t size)
{
    if (g_top == 0)
        g_top = g_end = (char*)sbrk(0);

    size = (size + 15) & ~(size_t)15;   /* 16-byte alignment */

    if (g_top + size > g_end) {
        size_t need = (size + 4095) & ~(size_t)4095;
        if (need < 65536)
            need = 65536;
        if (sbrk((long)need) == (void*)-1)
            return NULL;
        g_end += need;
    }

    void* block = g_top;
    g_top += size;
    return block;
}

void free(void* pointer)
{
    (void)pointer;
}

void* calloc(size_t count, size_t size)
{
    const size_t total = count * size;
    /* Guard against the multiply wrapping around. */
    if (size != 0 && total / size != count)
        return NULL;
    void* block = malloc(total);
    if (block != NULL)
        memset(block, 0, total);
    return block;
}

int atoi_simple(const char* text)
{
    int value = 0;
    for (; *text >= '0' && *text <= '9'; ++text)
        value = value * 10 + (*text - '0');
    return value;
}
