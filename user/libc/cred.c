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

int login(const char* user, const char* password, char* home)
{
    return (int)__syscall(SYS_login, (long)user, (long)password, (long)home, 0, 0);
}

void setecho(int on)
{
    __syscall(SYS_setecho, on, 0, 0, 0, 0);
}

int username(unsigned uid, char* name_out)
{
    return (int)__syscall(SYS_username, (long)uid, (long)name_out, 0, 0, 0);
}

int useradd(const char* name, const char* password, unsigned uid, unsigned gid,
            const char* home)
{
    return (int)__syscall(SYS_useradd, (long)name, (long)password, (long)uid,
                          (long)gid, (long)home);
}

int passwd(const char* name, const char* old_password, const char* new_password)
{
    return (int)__syscall(SYS_passwd, (long)name, (long)old_password,
                          (long)new_password, 0, 0);
}

int chmod(const char* path, unsigned mode)
{
    return (int)__syscall(SYS_chmod, (long)path, (long)mode, 0, 0, 0);
}

int chown(const char* path, unsigned uid, unsigned gid)
{
    return (int)__syscall(SYS_chown, (long)path, (long)uid, (long)gid, 0, 0);
}
