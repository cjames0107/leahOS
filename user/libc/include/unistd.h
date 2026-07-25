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

#endif /* _UNISTD_H */
