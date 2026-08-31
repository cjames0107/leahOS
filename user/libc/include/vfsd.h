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
#define VFS_SYMLINK 14  /* data = target\0path\0 -> path becomes a link to it  */
#define VFS_READLINK 15 /* data = path -> data = where it points, w0 = length  */
#define VFS_LSTAT   16  /* stat, but of the link itself rather than its target */
#define VFS_STATFS  17  /* -> w0 = block size, w1 = blocks, w2 = free blocks  */
#define VFS_LINK    18  /* data = target\0path\0 -> a second name, one inode  */
#define VFS_MKFIFO  19  /* data = path -> a name for a pipe, holding nothing  */
#define VFS_MKNOD   20  /* data = path, w1 = kind, w2 = rdev -> a device node */
#define VFS_MOUNT   22  /* data = mount point, w1 = disk -> attaches it       */
#define VFS_UMOUNT  23  /* data = mount point -> detaches it                  */
/* data = path, w1 = 1 to run it / 0 to map it as a library
 *   -> w0 = size, handle[0] = an image carrying the rights that were earned
 *
 * The one request that exists because of who is answering it. Execute
 * permission cannot be enforced anywhere else: execve is handed bytes and
 * never learns which file they came from, so a check in libc is the process
 * checking itself. Here the file, its mode bits and the caller's credentials
 * are all in one place, and what comes back is a capability rather than an
 * answer that has to be believed. */
#define VFS_EXECIMAGE 24

#define VFS_FSCK    21  /* w1 = repair -> w0 = problems, w1 = fixed, data =
                           the report as lines of text                      */

#define VFS_KIND_FILE 0
#define VFS_KIND_DIR  1
/* A name that holds another name. Everything above resolves these on the way
 * through, so a caller only ever sees this kind from LIST and LSTAT - the two
 * that are asking about the entry rather than about what it leads to. */
#define VFS_KIND_LINK 2
/* A name for a pipe. The file holds nothing and never will: it exists so the
 * pipe has a name, an owner and permissions, which is what a filesystem is
 * for. Everything that moves goes through the kernel, not the disk. */
#define VFS_KIND_FIFO 3

/* A name for a driver.
 *
 * The file holds nothing here either, but unlike a FIFO it is not empty: the
 * inode carries a device number, and that number is the whole content. Until
 * now libc decided what /dev/null was by comparing the path against a string,
 * which meant the device was a fact about the C library rather than about the
 * filesystem - a second name for it did nothing, and a file called /dev/null
 * on a mounted stranger's disk would have been swallowed by the rule.
 *
 * With the number on disk the identity travels with the inode, which is what
 * lets `ln /dev/null /tmp/sink` behave, and what makes `mknod` possible at all.
 */
#define VFS_KIND_CHR  4
#define VFS_KIND_BLK  5

/* Major and minor packed the way ext4 stores a small device in i_block[0]. */
#define VFS_MKDEV(maj, min) ((unsigned)(((maj) << 8) | ((min) & 0xFF)))
#define VFS_MAJOR(rdev)     (((rdev) >> 8) & 0xFF)
#define VFS_MINOR(rdev)     ((rdev) & 0xFF)

#endif
