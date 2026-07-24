#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

_Noreturn void exit(int status)
{
    __syscall(SYS_exit, status, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* A fixed arena carved out of .bss. No brk/mmap syscall exists yet, so the heap
 * cannot grow; when one does, this becomes a real allocator. Until then a
 * program that stays under the arena size gets working malloc, and free is a
 * deliberate no-op. */
#define ARENA_SIZE (256 * 1024)

static unsigned char g_arena[ARENA_SIZE];
static size_t g_offset = 0;

void* malloc(size_t size)
{
    /* 16-byte alignment, matching the widest scalar the ABI can hand back. */
    size = (size + 15) & ~(size_t)15;
    if (g_offset + size > ARENA_SIZE)
        return NULL;
    void* block = &g_arena[g_offset];
    g_offset += size;
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
