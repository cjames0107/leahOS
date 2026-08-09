#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>

/* Matches the kernel's UserStat / files::Dirent layout. */
#define S_IFREG 0
#define S_IFDIR 1
/* A name whose contents are another name. Only lstat and getdents ever report
 * one: everything else resolves it on the way through, which is the whole
 * point of it. */
#define S_IFLNK 2
/* A name for a pipe. The file holds nothing: it exists so the pipe has a name
 * two unrelated programs can both open, which is the one thing an ordinary
 * pipe cannot give them. */
#define S_IFIFO 3
/* A name for a driver. The inode holds a device number and nothing else, and
 * that number - not the path - is what says which driver. See the note beside
 * VFS_KIND_CHR in vfsd.h for why that distinction is worth having. */
#define S_IFCHR 4
#define S_IFBLK 5

struct stat {
    uint32_t st_type;       /* S_IFREG, S_IFDIR, S_IFLNK, S_IFIFO, S_IFCHR,
                               S_IFBLK */
    uint32_t st_ino;        /* which inode - unique, and how a FIFO is found */
    uint32_t st_mode;       /* permission bits, 0777 */
    uint32_t st_rdev;       /* which device, when st_type says it is one */
    uint64_t st_size;
    uint32_t st_uid;
    uint32_t st_gid;
    /* Seconds since 1970. mtime is when the contents last changed, ctime when
     * the inode did - chmod moves the second and not the first, which is what
     * lets a backup tell a real change from an adjusted permission. */
    int64_t  st_mtime;
    int64_t  st_ctime;
    int64_t  st_atime;
};

struct dirent {
    uint32_t d_type;        /* S_IFREG, S_IFDIR or S_IFLNK */
    uint32_t d_mode;        /* permission bits, 0777 - the execute bits are
                               how a program is told from a document */
    uint64_t d_size;
    int64_t  d_mtime;       /* seconds since 1970 */
    char     d_name[128];
};

/* Whether a mode says "this can be run". Any of the three bits: a file the
 * owner may execute is a program even if nobody else may. */
#define S_ISEXEC(mode) (((mode) & 0111u) != 0)

int stat(const char* path, struct stat* out);

/* The link itself rather than what it points at. The difference matters to
 * anything that walks a tree: `ls -l` wants to show the link, and `rm` wants
 * to remove the link, while everything else wants the file at the end of it. */
int lstat(const char* path, struct stat* out);

/* Make `path` a symbolic link holding the text `target`. The target is not
 * checked and need not exist - a link to nothing is a normal thing to have,
 * and is how a link to a filesystem that is not mounted yet behaves. */
int symlink(const char* target, const char* path);

/* A second name for a file that already exists: one inode, two directory
 * entries, and no way to tell which came first. Not a copy - writing through
 * one name changes what the other names. Directories are refused, because a
 * second name for one turns the tree into a graph and every walk of it into a
 * cycle waiting to happen. */
int link(const char* existing, const char* path);

/* What a link holds, without a terminating null - the length is the return,
 * as it is everywhere, because a target may legitimately contain anything.
 * -1 with EINVAL when the path is not a link. */
long readlink(const char* path, char* out, unsigned long max);

/* leahOS-specific: fill an array of dirents for a directory. Returns the count,
 * or -1. A real opendir/readdir pair can wrap this later. */
int getdents(const char* path, struct dirent* buffer, int max);

int mkdir(const char* path);

/* Make a name for a pipe.
 *
 * Two programs started separately cannot share an ordinary pipe: a pipe is
 * found by inheritance, and they have no common ancestor to inherit from. A
 * FIFO is the same pipe with a name on the filesystem, so both can find it.
 *
 * Opening one waits for the other end. That is deliberate and is what makes it
 * a rendezvous - without it `echo hi > fifo` would finish before anything was
 * there to receive what it wrote. */
int mkfifo(const char* path, unsigned mode);

/* Make a device node: a name whose inode carries a device number instead of
 * any contents. `type` is S_IFCHR or S_IFBLK, `rdev` is makedev(major, minor).
 *
 * Root only, and not because the inode is dangerous to write - it is four
 * bytes - but because the number in it is a claim about which driver answers.
 * A user who may mknod may make a file that reads as the disk. */
int mknod(const char* path, unsigned type, unsigned mode, unsigned rdev);

#define makedev(maj, min) ((unsigned)(((maj) << 8) | ((min) & 0xFF)))
#define major(rdev)       (((rdev) >> 8) & 0xFF)
#define minor(rdev)       ((rdev) & 0xFF)

/* Change a file's permission bits, or its owner. Only root, or the file's
 * owner, may chmod; only root may chown. -1 leaves a chown field alone. */
int chmod(const char* path, unsigned mode);
int chown(const char* path, unsigned uid, unsigned gid);

#endif /* _SYS_STAT_H */
