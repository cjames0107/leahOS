#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <stdint.h>

/* How big a filesystem is and how much of it is left.
 *
 * Blocks rather than bytes, as every UNIX reports it, because blocks are what
 * the filesystem actually allocates - a thousand one-byte files do not use a
 * thousand bytes, and a figure in bytes would say they did.
 */
struct statfs {
    uint64_t f_bsize;       /* bytes per block */
    uint64_t f_blocks;      /* how many there are */
    uint64_t f_bfree;       /* how many are unused */
};

/* Any path on the filesystem being asked about; it does not have to be the
 * mount point. Zeros in f_blocks mean "this is not storage" - /proc has no
 * size to report and should not be made to invent one. */
int statfs(const char* path, struct statfs* out);

#endif /* _SYS_STATFS_H */
