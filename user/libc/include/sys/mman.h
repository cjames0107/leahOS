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

/* msync flags. Accepted and largely alike: there is one way to write a
 * mapping back and it is synchronous, so MS_ASYNC does the same thing as
 * MS_SYNC rather than pretending to defer. MS_INVALIDATE is refused, because
 * discarding a mapping's changes is not something this can do. */
#define MS_ASYNC       1
#define MS_SYNC        2
#define MS_INVALIDATE  4

/* Map `length` bytes of zeroed anonymous memory. `addr` is a hint unless
 * MAP_FIXED is given. Returns MAP_FAILED on failure. */
/* Write a file mapping back to the file it came from.
 *
 * A mapping here is private and copy-on-write: the pages are the file's until
 * something writes to one, and then they are a copy. Nothing propagates on its
 * own, which is what makes this call the whole of writeback rather than a hint
 * that some of it has happened already.
 *
 * Deliberately explicit. A shared writable mapping - where a write reaches the
 * file without being asked to - would mean handing a process a writable
 * mapping of the pages the kernel holds for that file, and those are the same
 * pages every *other* process runs its program text from. That is not a
 * refinement away; it needs the file's pages and the image's pages to stop
 * being the same thing.
 *
 * `addr` and `length` name a range inside a mapping made by mmap from a
 * descriptor. Returns 0, or -1 with errno set. */
int msync(void* addr, size_t length, int flags);

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset);

/* Unmap a range. Returns 0, or -1. */
int munmap(void* addr, size_t length);

#endif /* _SYS_MMAN_H */
