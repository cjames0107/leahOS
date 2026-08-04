#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>

/* Matches the kernel's UserStat / files::Dirent layout. */
#define S_IFREG 0
#define S_IFDIR 1

struct stat {
    uint32_t st_type;       /* S_IFREG or S_IFDIR */
    uint32_t st_mode;       /* permission bits, 0777 */
    uint64_t st_size;
    uint32_t st_uid;
    uint32_t st_gid;
};

struct dirent {
    uint32_t d_type;        /* S_IFREG or S_IFDIR */
    uint32_t d_mode;        /* permission bits, 0777 - the execute bits are
                               how a program is told from a document */
    uint64_t d_size;
    char     d_name[128];
};

/* Whether a mode says "this can be run". Any of the three bits: a file the
 * owner may execute is a program even if nobody else may. */
#define S_ISEXEC(mode) (((mode) & 0111u) != 0)

int stat(const char* path, struct stat* out);

/* leahOS-specific: fill an array of dirents for a directory. Returns the count,
 * or -1. A real opendir/readdir pair can wrap this later. */
int getdents(const char* path, struct dirent* buffer, int max);

int mkdir(const char* path);

/* Change a file's permission bits, or its owner. Only root, or the file's
 * owner, may chmod; only root may chown. -1 leaves a chown field alone. */
int chmod(const char* path, unsigned mode);
int chown(const char* path, unsigned uid, unsigned gid);

#endif /* _SYS_STAT_H */
