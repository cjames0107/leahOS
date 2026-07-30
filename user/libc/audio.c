#include <audio.h>
#include <sys/syscall.h>

int audio_info(struct audio_info* out)
{
    return (int)__syscall(SYS_audioinfo, (long)out, 0, 0, 0, 0);
}

long audio_play(const int16_t* samples, long count)
{
    return __syscall(SYS_audioplay, (long)samples, count, 0, 0, 0);
}

long audio_space(void)
{
    return __syscall(SYS_audiospace, 0, 0, 0, 0, 0);
}

int audio_volume(void)
{
    /* A negative argument reads without setting - see the kernel side. */
    return (int)__syscall(SYS_audiovol, -1, 0, 0, 0, 0);
}

int audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    return (int)__syscall(SYS_audiovol, percent, 0, 0, 0, 0);
}

void audio_flush(void)
{
    __syscall(SYS_audioflush, 0, 0, 0, 0, 0);
}

void audio_stop(void)
{
    __syscall(SYS_audiostop, 0, 0, 0, 0, 0);
}
