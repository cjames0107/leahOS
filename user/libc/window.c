#include <sys/syscall.h>
#include <window.h>

int win_create(int x, int y, unsigned width, unsigned height, const char* title)
{
    return (int)__syscall(SYS_wincreate, x, y, (long)width, (long)height,
                          (long)title);
}

uint32_t* win_map(int id)
{
    long address = __syscall(SYS_winmap, id, 0, 0, 0, 0);
    if (address == -1)
        return 0;
    return (uint32_t*)address;
}

void win_present(int id) { __syscall(SYS_winpresent, id, 0, 0, 0, 0); }

int win_poll(int id, struct win_event* out)
{
    return (int)__syscall(SYS_winpoll, id, (long)out, 0, 0, 0);
}

void win_destroy(int id) { __syscall(SYS_windestroy, id, 0, 0, 0, 0); }
