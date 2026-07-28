#include <display.h>
#include <sys/syscall.h>

int fb_info(struct fb_info* out)
{
    return (int)__syscall(SYS_fbinfo, (long)out, 0, 0, 0, 0);
}

void* fb_map(void)
{
    long address = __syscall(SYS_fbmap, 0, 0, 0, 0, 0);
    if (address == -1)
        return 0;
    return (void*)address;
}

int input_poll(struct input_state* out)
{
    return (int)__syscall(SYS_inputpoll, (long)out, 0, 0, 0, 0);
}

int fb_font(unsigned char* out)
{
    return (int)__syscall(SYS_fbfont, (long)out, 0, 0, 0, 0);
}
