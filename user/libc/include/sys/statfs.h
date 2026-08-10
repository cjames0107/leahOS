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


/* Check the mounted filesystem, and optionally put right what can be put
 * right. Returns the number of problems found, or -1 if the check could not
 * run; `report` is filled with the findings as lines of text and `fixed` with
 * how many of the problems were repaired.
 *
 * The check runs inside the filesystem server, which is the only thing that
 * knows the layout. That means it cannot examine a filesystem that failed to
 * mount - the case a checker is most wanted for - and it sees the disk as the
 * server currently believes it to be. */
long fsck(int repair, char* report, unsigned long max, unsigned* fixed);


/* Attach the filesystem on `disk` at `at`, or detach whatever is there.
 *
 * The disk is an index, because that is all the block driver knows about
 * itself: zero is the one the system booted from. Root only - attaching a
 * disk puts somebody else's idea of who owns which file into this tree. */
int fs_mount(unsigned disk, const char* at);
int fs_umount(const char* at);

#endif /* _SYS_STATFS_H */
