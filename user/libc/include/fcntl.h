#ifndef _FCNTL_H
#define _FCNTL_H

/* open() flags. Values match the kernel's files::k* constants. */
#define O_RDONLY  1
#define O_WRONLY  2
#define O_RDWR    3
#define O_CREAT   4
#define O_TRUNC   8
#define O_APPEND  16

int open(const char* path, int flags);

#endif /* _FCNTL_H */
