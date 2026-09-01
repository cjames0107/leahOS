#include <fcntl.h>
#include <errno.h>
#include <loader.h>
#include <object.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* What each file mapping came from.
 *
 * The kernel is not asked, and could not answer: it maps frames from a held
 * image and keeps no note of which descriptor a process used to ask for it.
 * Writing a mapping back needs the path and the offset, and this is the only
 * side that ever knew them.
 *
 * Small and fixed. A program with more than eight files mapped at once is not
 * something this system has, and a table that could grow would need a lock the
 * moment threads used it. */
#define MAPPED_MAX 8

static struct {
    unsigned long base;
    unsigned long bytes;
    long          offset;
    char          path[192];
} g_mapped[MAPPED_MAX];

static void remember(unsigned long base, unsigned long bytes, long offset,
                     const char* path)
{
    for (int i = 0; i < MAPPED_MAX; ++i) {
        if (g_mapped[i].bytes != 0)
            continue;
        g_mapped[i].base = base;
        g_mapped[i].bytes = bytes;
        g_mapped[i].offset = offset;
        unsigned n = 0;
        while (path[n] != '\0' && n < sizeof(g_mapped[i].path) - 1) {
            g_mapped[i].path[n] = path[n];
            ++n;
        }
        g_mapped[i].path[n] = '\0';
        return;
    }
    /* Full: the mapping still works, it just cannot be written back. Silent
     * because failing the mmap over it would be worse - the caller asked to
     * read a file, and reading it is unaffected. */
}

int msync(void* addr, size_t length, int flags)
{
    if ((flags & MS_INVALIDATE) != 0) {
        errno = EINVAL;             /* nothing here can discard changes */
        return -1;
    }
    const unsigned long at = (unsigned long)addr;
    for (int i = 0; i < MAPPED_MAX; ++i) {
        if (g_mapped[i].bytes == 0)
            continue;
        if (at < g_mapped[i].base || at >= g_mapped[i].base + g_mapped[i].bytes)
            continue;

        /* Clipped to the mapping: a caller may name a range inside it, and
         * writing past the end would append to the file. */
        const unsigned long within = at - g_mapped[i].base;
        unsigned long span = length;
        if (span > g_mapped[i].bytes - within)
            span = g_mapped[i].bytes - within;

        const int fd = open(g_mapped[i].path, O_WRONLY);
        if (fd < 0)
            return -1;
        if (lseek(fd, g_mapped[i].offset + (long)within, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
        unsigned long done = 0;
        while (done < span) {
            const long n = write(fd, (const char*)addr + done, span - done);
            if (n <= 0) {
                close(fd);
                errno = EIO;
                return -1;
            }
            done += (unsigned long)n;
        }
        close(fd);
        return 0;
    }
    errno = ENOMEM;                 /* not a mapping this library made */
    return -1;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset)
{
    /* A file, if one was named.
     *
     * mmap took a descriptor and ignored it until now, because the kernel had
     * no way to map anything but fresh anonymous pages. It maps a held image
     * instead: the same pages a program's text has been mapped from since the
     * image cache landed, offered to anything that can name a file.
     *
     * The mapping is private whatever was asked for. Writing back to the file
     * would need the image to be more than the snapshot it is, so a write
     * takes a private copy - which is MAP_PRIVATE, and is what mapping a file
     * in order to read it wants. */
    if (fd >= 0 && (flags & MAP_ANONYMOUS) == 0) {
        const char* path = __fd_path(fd);
        long size = 0;
        if (path == 0) {
            errno = ENODEV;
            return MAP_FAILED;
        }
        const int image = __vfs_image(path, 0, &size);
        if (image < 0)
            return MAP_FAILED;
        const long at = __syscall(SYS_mapfile, (long)addr, (long)length,
                                  prot, image, offset);
        obj_close(image);
        if (at < 0) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        remember((unsigned long)at, (unsigned long)length, offset, path);
        return (void*)at;
    }

    (void)fd;
    (void)offset;
    long r = __syscall(SYS_mmap, (long)addr, (long)length, prot, flags, 0);
    if (r == -1)
        return MAP_FAILED;
    return (void*)r;
}

int munmap(void* addr, size_t length)
{
    return (int)__syscall(SYS_munmap, (long)addr, (long)length, 0, 0, 0);
}
