#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

unsigned getuid(void) { return (unsigned)__syscall(SYS_getuid, 0, 0, 0, 0, 0); }
unsigned getgid(void) { return (unsigned)__syscall(SYS_getgid, 0, 0, 0, 0, 0); }

int setuid(unsigned uid)
{
    return (int)__syscall(SYS_setuid, (long)uid, 0, 0, 0, 0);
}

int setgid(unsigned gid)
{
    return (int)__syscall(SYS_setgid, (long)gid, 0, 0, 0, 0);
}

int chmod(const char* path, unsigned mode)
{
    return (int)__syscall(SYS_chmod, (long)path, (long)mode, 0, 0, 0);
}

int chown(const char* path, unsigned uid, unsigned gid)
{
    return (int)__syscall(SYS_chown, (long)path, (long)uid, (long)gid, 0, 0);
}
