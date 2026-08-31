#include <errno.h>
#include <loader.h>
#include <object.h>
#include <sys/mman.h>
#include <sys/syscall.h>

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
