#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>

/* Protection bits, mirrored from kernel/include/leah/syscall.hpp. */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* Mapping flags. Only private anonymous mappings are supported: file-backed
 * mmap needs a page cache to be worth having. */
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void*)-1)

/* Map `length` bytes of zeroed anonymous memory. `addr` is a hint unless
 * MAP_FIXED is given. Returns MAP_FAILED on failure. */
void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset);

/* Unmap a range. Returns 0, or -1. */
int munmap(void* addr, size_t length);

#endif /* _SYS_MMAN_H */
