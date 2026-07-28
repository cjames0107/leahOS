#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef int  pid_t;

ssize_t write(int fd, const void* buffer, size_t count);
ssize_t read(int fd, void* buffer, size_t count);
pid_t   getpid(void);

pid_t fork(void);
int   execve(const char* path, char* const argv[], char* const envp[]);
pid_t wait(int* status);
void  yield(void);

/* Block for at least `ms` milliseconds. Prefer this to spinning on yield() in a
 * polling loop: a task that only yields stays runnable and keeps taking the
 * kernel lock, which on a multiprocessor can starve another CPU out of it. */
void  msleep(unsigned long ms);

int    close(int fd);
long   lseek(int fd, long offset, int whence);
int    chdir(const char* path);
int    getcwd(char* buffer, size_t size);
int    unlink(const char* path);
int    pipe(int fds[2]);
int    dup2(int oldfd, int newfd);
int    rename(const char* oldpath, const char* newpath);
void*  sbrk(long increment);

/* Credentials. uid 0 is root; only root may change them. */
unsigned getuid(void);
unsigned getgid(void);
int      setuid(unsigned uid);
int      setgid(unsigned gid);

/* Authenticate as `user` and, on success, become them. The password is checked
 * inside the kernel against a shadow file no user process can read, so this
 * needs no setuid bit. Pass a null password as root, which is not asked for one.
 * `home` receives the account's home directory. Returns 0, or -1. */
int login(const char* user, const char* password, char* home);

/* Console echo, off while a password is being typed. */
void setecho(int on);

/* The account name for a uid. Returns 0, or -1 when there is no such account. */
int username(unsigned uid, char* name_out);

/* Create an account. Root only. The kernel hashes the password and creates the
 * home directory; nothing here ever sees a digest. Returns 0, or -1. */
int useradd(const char* name, const char* password, unsigned uid, unsigned gid,
            const char* home);

/* Change a password. Root may change anyone's and may pass a null `old`;
 * anyone else may change only their own and must prove it. Returns 0, or -1. */
int passwd(const char* name, const char* old_password, const char* new_password);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* _UNISTD_H */
