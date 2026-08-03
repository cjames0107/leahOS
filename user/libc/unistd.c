#include <sys/syscall.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* read and write live in fs.c now, with the descriptor table they have to
 * consult: which of them is a file and which is the console is a fact about
 * that table, and the table is ours. */

pid_t getpid(void)
{
    return (pid_t)__syscall(SYS_getpid, 0, 0, 0, 0, 0);
}

pid_t fork(void)
{
    return (pid_t)__syscall(SYS_fork, 0, 0, 0, 0, 0);
}

/* Read the program, then hand the kernel its bytes.
 *
 * The kernel used to open the path itself, which made it a client of the
 * filesystem server in the middle of building an address space - a blocking
 * call at the worst possible moment, and the last place ring 0 reached into
 * ring 3 for something it needed. Whoever wants to run a program is already a
 * process with a filesystem; it can do its own reading.
 */
int execve(const char* path, char* const argv[], char* const envp[])
{
    char full[256];
    struct stat st;
    int fd;
    void* image;
    long got;
    int result;

    (void)envp;

    __fd_resolve(path, full);
    if (stat(full, &st) != 0 || st.st_type != S_IFREG || st.st_size == 0)
        return -1;

    fd = open(full, O_RDONLY);
    if (fd < 0)
        return -1;
    image = malloc((size_t)st.st_size);
    if (image == 0) {
        close(fd);
        return -1;
    }
    got = read(fd, image, (unsigned long)st.st_size);
    close(fd);
    if (got != (long)st.st_size) {
        free(image);
        return -1;
    }

    /* Last, because it has to reflect the table as it will be handed over -
     * after the descriptor this function opened has been closed again. */
    __fd_save_for_exec();

    /* Work out what the segments are here. The kernel maps what it is told
     * and no longer knows what an ELF is - the only programs it ever loaded
     * itself are the three the build hands it, already parsed. */
    {
        struct seg { unsigned long vaddr, offset, filesz, memsz;
                     unsigned flags, pad; };
        struct { unsigned long entry; unsigned count, pad;
                 struct seg segs[16]; } req;
        struct seg* segs = req.segs;
        const unsigned char* e = (const unsigned char*)image;
        unsigned long phoff;
        unsigned short phentsize, phnum, i;
        unsigned count = 0;
        unsigned long entry;

        if (got < 64 || e[0] != 0x7F || e[1] != 'E' || e[2] != 'L' ||
            e[3] != 'F' || e[4] != 2 || e[5] != 1) {
            free(image);
            return -1;
        }
        memcpy(&entry, e + 24, 8);
        memcpy(&phoff, e + 32, 8);
        memcpy(&phentsize, e + 54, 2);
        memcpy(&phnum, e + 56, 2);

        for (i = 0; i < phnum && count < 16; ++i) {
            const unsigned char* ph = e + phoff + (unsigned long)i * phentsize;
            unsigned p_type, p_flags;
            unsigned long p_offset, p_vaddr, p_filesz, p_memsz;
            if ((unsigned long)(ph - e) + phentsize > (unsigned long)got)
                break;
            memcpy(&p_type, ph, 4);
            memcpy(&p_flags, ph + 4, 4);
            memcpy(&p_offset, ph + 8, 8);
            memcpy(&p_vaddr, ph + 16, 8);
            memcpy(&p_filesz, ph + 32, 8);
            memcpy(&p_memsz, ph + 40, 8);
            if (p_type != 1 || p_memsz == 0)
                continue;               /* only PT_LOAD carries anything */
            if (p_filesz > p_memsz || p_offset + p_filesz > (unsigned long)got) {
                free(image);
                return -1;
            }
            segs[count].vaddr  = p_vaddr;
            segs[count].offset = p_offset;
            segs[count].filesz = p_filesz;
            segs[count].memsz  = p_memsz;
            segs[count].flags  = p_flags;
            segs[count].pad    = 0;
            ++count;
        }
        if (count == 0) {
            free(image);
            return -1;
        }
        req.entry = entry;
        req.count = count;
        req.pad   = 0;
        result = (int)__syscall(SYS_execve, (long)image, got, (long)argv,
                                (long)&req, 0);
    }

    /* Only reached when the exec failed: on success the kernel never returns
     * here, and this address space is already gone. */
    free(image);
    return result;
}

pid_t wait(int* status)
{
    return (pid_t)__syscall(SYS_wait, 0, (long)status, 0, 0, 0);
}

void msleep(unsigned long ms) { __syscall(SYS_sleep, (long)ms, 0, 0, 0, 0); }

void yield(void)
{
    __syscall(SYS_yield, 0, 0, 0, 0, 0);
}
