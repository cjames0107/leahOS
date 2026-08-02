#ifndef _VFSD_H
#define _VFSD_H

/* The filesystem, as seen from outside the process that implements it.
 *
 * Paths in, bytes out. There are no file descriptors here: a descriptor is a
 * per-process table, and whose table it is belongs to the kernel rather than
 * to the filesystem. So these calls name a file every time and carry an
 * offset, and the descriptor stays where it always was.
 *
 * Bulk data goes through the same shared segment the disk driver uses a level
 * further down - one copy at each boundary, which is one more than a monolith
 * and the price of the boundary being real.
 */

#define VFS_SHM_KEY  0x5646u        /* "VF" */
#define VFS_CHUNK    8192           /* the most one read or write moves */

struct vfs_shared {
    unsigned char data[VFS_CHUNK];
};

#define VFS_STAT     1  /* data = path -> w0 = size, w1 = kind, w2 = mode      */
#define VFS_READ     2  /* data = path, w1 = offset, w2 = length -> w0 = read  */
#define VFS_LIST     3  /* data = path, w1 = index -> name, w0 = kind, w1 = size */
#define VFS_MOUNTED  4  /* -> w0 = 1 when a filesystem is up, w1 = block size  */

#define VFS_CREATE   5  /* data = path -> makes an empty file                */
#define VFS_WRITE    6  /* data = path, w1 = offset, w2 = length <- segment   */
#define VFS_MKDIR    7  /* data = path                                        */
#define VFS_UNLINK   8  /* data = path                                        */
#define VFS_SYNC     9  /* flush what is held back                            */
#define VFS_CHMOD   10  /* data = path, w1 = mode                             */
#define VFS_CHOWN   11  /* data = path, w1 = uid, w2 = gid                    */
#define VFS_TRUNC   12  /* data = path -> length zero, blocks returned        */
#define VFS_RENAME  13  /* data = old\0new\0 -> the entry moves, inode stays   */

#define VFS_KIND_FILE 0
#define VFS_KIND_DIR  1

#endif
