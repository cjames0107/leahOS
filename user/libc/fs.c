/* File descriptors, on the near side of the boundary.
 *
 * A descriptor used to be a kernel object: the kernel held the table, resolved
 * the path, checked the permission and forwarded the work to vfsd. That put the
 * kernel in the middle of every read - as a *client* of a userland server,
 * blocking in a system call waiting for a reply. A microkernel should not be
 * anywhere in that path.
 *
 * So the table is here. An open file is a path and an offset, which is all it
 * ever was: vfsd names a file in every message and keeps no per-client state,
 * so nothing had to be invented for this - the descriptor moved to the side
 * that was already keeping the offset.
 *
 * Three kinds share one table:
 *
 *   K_FILE   - a path and where we are in it. Talks to vfsd.
 *   K_KERNEL - the console, and pipes. Those are genuinely kernel objects: a
 *              pipe is a rendezvous between two processes, the console is a
 *              device, and both need something that can put a task to sleep
 *              and wake it. The entry holds the kernel's own number.
 *   K_NONE   - free.
 *
 * The numbering is ours. That is exactly what lets 0, 1 and 2 be redirected to
 * a file without the kernel having to know what a file is.
 *
 * Nothing here is trusted for permission. libc is the process's own code, so a
 * check made here is the process checking itself; open consults the mode bits
 * only so it can fail early and honestly. vfsd checks every read and write for
 * real, on the other side of the boundary.
 */

#include <fcntl.h>
#include <ipc.h>
#include <shm.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vfsd.h>

#define FD_MAX    32
#define PATH_MAX  256

#define K_NONE   0
#define K_KERNEL 1
#define K_FILE   2

struct entry {
    unsigned char kind;
    int           kfd;          /* K_KERNEL: the number the kernel knows */
    long          offset;       /* K_FILE */
    unsigned      flags;
    char          path[PATH_MAX];
};

static struct entry g_fds[FD_MAX];
static char         g_cwd[PATH_MAX] = "/";
static int          g_started;

/* --- talking to vfsd ---------------------------------------------------------
 *
 * The transfer buffer is this process's own, named in every message. One
 * segment shared by every client would be overwritten between a reply arriving
 * and this process copying out of it. */
static int g_vfs = -2;                  /* -2 not tried, -1 no server */
static struct vfs_shared* g_buf;
static unsigned g_buf_key;
static int g_buf_pid = -1;              /* whose segment g_buf actually is */

static int vfs_port(void)
{
    const int pid = (int)getpid();
    int id;

    /* The pid is checked every time, not just on the first call, because of
     * fork. A forked child inherits these statics: the parent's key, and a
     * mapping of the parent's segment that points at the same physical pages.
     * Nothing fails - both processes just use one buffer, and each reads what
     * the other left in it. Whole files come back as fragments of whatever
     * the other one was doing.
     *
     * exec hides this, which is why it took a while to find: fork+exec resets
     * the statics, so every shell command was fine and only long-lived
     * processes that fork without exec - which is most of the desktop - were
     * corrupting each other. */
    if (g_vfs != -2 && g_buf_pid == pid)
        return g_vfs;

    g_vfs = port_open(IPC_PORT_VFS);
    g_buf = 0;
    g_buf_pid = pid;
    if (g_vfs < 0)
        return g_vfs;

    g_buf_key = 0x56460000u | ((unsigned)pid & 0xFFFFu);
    id = shm_open(g_buf_key, sizeof(struct vfs_shared), SHM_PUBLIC);
    g_buf = id < 0 ? 0 : (struct vfs_shared*)shm_map(id);
    if (g_buf == 0)
        g_vfs = -1;
    return g_vfs;
}

static int vfs_call(unsigned tag, const char* path, long w1, long w2,
                    struct ipc_message* reply)
{
    struct ipc_message q;
    const int port = vfs_port();
    unsigned n = 0;

    if (port < 0)
        return -1;
    memset(&q, 0, sizeof(q));
    q.tag = tag;
    q.word[1] = w1;
    q.word[2] = w2;
    q.shm_key = (int)g_buf_key;
    q.shm_bytes = sizeof(struct vfs_shared);
    if (path != 0)
        while (path[n] != '\0' && n + 1 < sizeof(q.data)) {
            q.data[n] = path[n];
            ++n;
        }
    q.data[n] = '\0';
    q.bytes = n + 1;

    memset(reply, 0, sizeof(*reply));
    return ipc_call(port, &q, reply);
}

/* --- paths -------------------------------------------------------------------
 *
 * Resolution moved out with the table, because a relative path only means
 * something beside a working directory and the working directory is ours now.
 * "." and ".." are folded here rather than by vfsd: they are facts about how a
 * name was written, not about what is on the disk. */
void __fd_resolve(const char* path, char* out)
{
    char work[PATH_MAX * 2];
    unsigned n = 0, i = 0, len;

    if (path == 0 || path[0] == '\0')
        path = ".";

    if (path[0] == '/') {
        while (path[n] != '\0' && n + 1 < sizeof(work)) { work[n] = path[n]; ++n; }
    } else {
        while (g_cwd[n] != '\0' && n + 1 < sizeof(work)) { work[n] = g_cwd[n]; ++n; }
        if (n > 0 && work[n - 1] != '/' && n + 1 < sizeof(work))
            work[n++] = '/';
        while (*path != '\0' && n + 1 < sizeof(work))
            work[n++] = *path++;
    }
    work[n] = '\0';
    len = n;

    /* Rebuilt a component at a time, so ".." can pop what came before it. */
    n = 0;
    out[n++] = '/';
    while (i < len) {
        unsigned start, seg, k;
        while (i < len && work[i] == '/') ++i;
        start = i;
        while (i < len && work[i] != '/') ++i;
        seg = i - start;
        if (seg == 0 || (seg == 1 && work[start] == '.'))
            continue;
        if (seg == 2 && work[start] == '.' && work[start + 1] == '.') {
            while (n > 1 && out[n - 1] != '/') --n;
            if (n > 1) --n;                 /* the separator goes too */
            continue;
        }
        if (n > 1 && n + 1 < PATH_MAX)
            out[n++] = '/';
        for (k = 0; k < seg && n + 1 < PATH_MAX; ++k)
            out[n++] = work[start + k];
    }
    out[n] = '\0';
}

/* --- the table across an exec ------------------------------------------------
 *
 * execve replaces the address space, so a table in memory does not survive it -
 * and it has to, or a shell redirecting a child's output would hand that child
 * nothing. It travels in a segment keyed by this process's own pid, which is
 * the one name both images agree on and does not require the kernel to learn
 * what a descriptor is. */
#define STATE_KEY(pid) (0xFD000000u | ((unsigned)(pid) & 0xFFFFu))
#define SAVED_MAGIC    0x4C464453u   /* "LFDS" */

struct saved {
    unsigned     magic;
    char         cwd[PATH_MAX];
    struct entry fds[FD_MAX];
};

static void start(void)
{
    int id;
    struct saved* s;

    if (g_started)
        return;
    g_started = 1;

    /* Whatever the image that called execve left for us. */
    id = shm_open(STATE_KEY(getpid()), sizeof(struct saved), SHM_PUBLIC);
    s = id < 0 ? 0 : (struct saved*)shm_map(id);
    if (s != 0 && s->magic == SAVED_MAGIC) {
        memcpy(g_fds, s->fds, sizeof(g_fds));
        memcpy(g_cwd, s->cwd, sizeof(g_cwd));
        s->magic = 0;           /* once: a later process must not inherit it */
        shm_destroy(id);
        return;
    }
    if (id >= 0)
        shm_destroy(id);

    /* A fresh process: the three the kernel opened for us, and the root. */
    g_fds[0].kind = K_KERNEL; g_fds[0].kfd = 0;
    g_fds[1].kind = K_KERNEL; g_fds[1].kfd = 1;
    g_fds[2].kind = K_KERNEL; g_fds[2].kfd = 2;
}

/* Called from execve, with the address space about to go away. */
void __fd_save_for_exec(void)
{
    int id;
    struct saved* s;

    start();
    id = shm_open(STATE_KEY(getpid()), sizeof(struct saved), SHM_PUBLIC);
    if (id < 0)
        return;
    s = (struct saved*)shm_map(id);
    if (s == 0)
        return;
    memcpy(s->fds, g_fds, sizeof(g_fds));
    memcpy(s->cwd, g_cwd, sizeof(g_cwd));
    s->magic = SAVED_MAGIC;
}

static int valid(int fd)
{
    return fd >= 0 && fd < FD_MAX && g_fds[fd].kind != K_NONE;
}

static int alloc_fd(void)
{
    int fd;
    for (fd = 0; fd < FD_MAX; ++fd)
        if (g_fds[fd].kind == K_NONE)
            return fd;
    return -1;
}

/* How many of our entries name the same kernel object. dup2 copies an entry
 * rather than asking the kernel for a second number, so the kernel's own
 * descriptor must not be closed until the last of ours goes. */
static int kernel_refs(int kfd)
{
    int fd, n = 0;
    for (fd = 0; fd < FD_MAX; ++fd)
        if (g_fds[fd].kind == K_KERNEL && g_fds[fd].kfd == kfd)
            ++n;
    return n;
}

/* The mode bits, read the way the far side will read them. Not a security
 * decision: see the call site. */
static int permitted_stat(unsigned mode, unsigned uid, unsigned gid, int want_write)
{
    const unsigned me = getuid();
    unsigned r, w;

    if (me == 0)
        return 1;
    if (uid == me)      { r = 0400; w = 0200; }
    else if (gid == getgid()) { r = 0040; w = 0020; }
    else                { r = 0004; w = 0002; }
    return (mode & (want_write ? w : r)) != 0;
}

/* --- the calls ---------------------------------------------------------------- */

int open(const char* path, int flags)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    int fd, exists, is_dir = 0;
    long size = 0;

    start();
    __fd_resolve(path, resolved);

    exists = vfs_call(VFS_STAT, resolved, 0, 0, &a) == 0 && a.word[0] >= 0;
    if (exists) {
        size   = a.word[0];
        is_dir = a.word[1] == VFS_KIND_DIR;
    }

    if (!exists) {
        if ((flags & O_CREAT) == 0)
            return -1;
        if (vfs_call(VFS_CREATE, resolved, 0, 0, &a) != 0 || a.word[0] < 0)
            return -1;
        size = 0;
    } else if (is_dir && (flags & O_WRONLY) != 0) {
        return -1;                      /* a directory cannot be written */
    } else if (!permitted_stat((unsigned)a.word[2],
                          (unsigned)((a.word[3] >> 16) & 0xFFFF),
                          (unsigned)(a.word[3] & 0xFFFF),
                          (flags & O_WRONLY) != 0)) {
        /* Advisory, and deliberately so. This is the process asking itself,
         * which decides nothing - vfsd checks every read and write on the far
         * side of the boundary. It is here because open() answering "yes" and
         * the first read answering "no" is a worse interface than open()
         * saying so, and because that is what open() has always done. */
        return -1;
    }

    if ((flags & O_TRUNC) != 0 && exists && !is_dir && size > 0)
        vfs_call(VFS_TRUNC, resolved, 0, 0, &a);

    fd = alloc_fd();
    if (fd < 0)
        return -1;

    g_fds[fd].kind   = K_FILE;
    g_fds[fd].flags  = (unsigned)flags;
    g_fds[fd].offset = (flags & O_APPEND) != 0 ? size : 0;
    memcpy(g_fds[fd].path, resolved, PATH_MAX);
    return fd;
}

int close(int fd)
{
    start();
    if (!valid(fd))
        return -1;
    if (g_fds[fd].kind == K_KERNEL && kernel_refs(g_fds[fd].kfd) == 1 &&
        g_fds[fd].kfd > 2)
        __syscall(SYS_close, g_fds[fd].kfd, 0, 0, 0, 0);
    g_fds[fd].kind = K_NONE;
    return 0;
}

long read(int fd, void* buffer, unsigned long count)
{
    struct ipc_message a;
    struct entry* e;
    long done = 0;

    start();
    if (!valid(fd))
        return -1;
    e = &g_fds[fd];
    if (e->kind == K_KERNEL)
        return __syscall(SYS_read, e->kfd, (long)buffer, (long)count, 0, 0);

    while ((unsigned long)done < count) {
        unsigned long want = count - (unsigned long)done;
        long got;
        if (want > VFS_CHUNK)
            want = VFS_CHUNK;
        if (vfs_call(VFS_READ, e->path, e->offset + done, (long)want, &a) != 0)
            return done > 0 ? done : -1;
        got = a.word[0];
        if (got <= 0)
            break;                      /* end of file, or refused */
        memcpy((char*)buffer + done, g_buf->data, (unsigned long)got);
        done += got;
        if ((unsigned long)got < want)
            break;
    }
    e->offset += done;
    return done;
}

long write(int fd, const void* buffer, unsigned long count)
{
    struct ipc_message a;
    struct entry* e;
    long done = 0;

    start();
    if (!valid(fd))
        return -1;
    e = &g_fds[fd];
    if (e->kind == K_KERNEL)
        return __syscall(SYS_write, e->kfd, (long)buffer, (long)count, 0, 0);

    /* The buffer has to exist before anything is copied into it. read() gets
     * away without this because it copies out *after* the call that maps it;
     * write copies in first, and on the first write of a process there was
     * nothing there yet. */
    if (vfs_port() < 0 || g_buf == 0)
        return -1;

    while ((unsigned long)done < count) {
        unsigned long want = count - (unsigned long)done;
        long put;
        if (want > VFS_CHUNK)
            want = VFS_CHUNK;
        memcpy(g_buf->data, (const char*)buffer + done, want);
        if (vfs_call(VFS_WRITE, e->path, e->offset + done, (long)want, &a) != 0)
            return done > 0 ? done : -1;
        put = a.word[0];
        if (put <= 0)
            break;
        done += put;
    }
    e->offset += done;
    return done;
}

long lseek(int fd, long offset, int whence)
{
    struct ipc_message a;
    struct entry* e;

    start();
    if (!valid(fd) || g_fds[fd].kind != K_FILE)
        return -1;
    e = &g_fds[fd];

    if (whence == SEEK_SET)
        e->offset = offset;
    else if (whence == SEEK_CUR)
        e->offset += offset;
    else if (whence == SEEK_END) {
        if (vfs_call(VFS_STAT, e->path, 0, 0, &a) != 0 || a.word[0] < 0)
            return -1;
        e->offset = a.word[0] + offset;
    } else
        return -1;

    if (e->offset < 0)
        e->offset = 0;
    return e->offset;
}

int stat(const char* path, struct stat* out)
{
    char resolved[PATH_MAX];
    struct ipc_message a;

    start();
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_STAT, resolved, 0, 0, &a) != 0 || a.word[0] < 0)
        return -1;

    out->st_size = (uint64_t)a.word[0];
    out->st_type = a.word[1] == VFS_KIND_DIR ? S_IFDIR : S_IFREG;
    out->st_mode = (uint32_t)a.word[2];
    out->st_uid  = (uint32_t)((a.word[3] >> 16) & 0xFFFF);
    out->st_gid  = (uint32_t)(a.word[3] & 0xFFFF);
    return 0;
}

int getdents(const char* path, struct dirent* buffer, int max)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    int n = 0;

    start();
    __fd_resolve(path, resolved);
    while (n < max) {
        unsigned i = 0;
        if (vfs_call(VFS_LIST, resolved, n, 0, &a) != 0 || a.word[0] < 0)
            break;
        while (i + 1 < sizeof(buffer[n].d_name) && a.data[i] != '\0') {
            buffer[n].d_name[i] = a.data[i];
            ++i;
        }
        buffer[n].d_name[i] = '\0';
        if (i == 0)
            break;
        buffer[n].d_type = a.word[0] == VFS_KIND_DIR ? S_IFDIR : S_IFREG;
        buffer[n].d_size = (uint64_t)a.word[1];
        buffer[n].d_reserved = 0;
        ++n;
    }
    return n;
}

int chdir(const char* path)
{
    char resolved[PATH_MAX];
    struct ipc_message a;

    start();
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_STAT, resolved, 0, 0, &a) != 0 || a.word[0] < 0 ||
        a.word[1] != VFS_KIND_DIR)
        return -1;
    memcpy(g_cwd, resolved, PATH_MAX);
    return 0;
}

int getcwd(char* buffer, size_t size)
{
    size_t i = 0;
    start();
    while (g_cwd[i] != '\0' && i + 1 < size) { buffer[i] = g_cwd[i]; ++i; }
    buffer[i] = '\0';
    return (int)i;
}

int mkdir(const char* path)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    start();
    __fd_resolve(path, resolved);
    return vfs_call(VFS_MKDIR, resolved, 0, 0, &a) == 0 && a.word[0] >= 0 ? 0 : -1;
}

int unlink(const char* path)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    start();
    __fd_resolve(path, resolved);
    return vfs_call(VFS_UNLINK, resolved, 0, 0, &a) == 0 && a.word[0] >= 0 ? 0 : -1;
}

int rename(const char* oldpath, const char* newpath)
{
    /* Still the kernel's, because vfsd has no rename and adding one is a change
     * to the filesystem rather than to where descriptors live. It is handed an
     * absolute path, so it no longer depends on a working directory the kernel
     * has stopped being told about. */
    char from[PATH_MAX], to[PATH_MAX];
    start();
    __fd_resolve(oldpath, from);
    __fd_resolve(newpath, to);
    return (int)__syscall(SYS_rename, (long)from, (long)to, 0, 0, 0);
}

int chmod(const char* path, unsigned mode)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    start();
    __fd_resolve(path, resolved);
    return vfs_call(VFS_CHMOD, resolved, (long)mode, 0, &a) == 0 &&
           a.word[0] >= 0 ? 0 : -1;
}

int chown(const char* path, unsigned uid, unsigned gid)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    start();
    __fd_resolve(path, resolved);
    return vfs_call(VFS_CHOWN, resolved, (long)uid, (long)gid, &a) == 0 &&
           a.word[0] >= 0 ? 0 : -1;
}

int pipe(int fds[2])
{
    int kfds[2];
    int r, w;

    start();
    if (__syscall(SYS_pipe, (long)kfds, 0, 0, 0, 0) != 0)
        return -1;

    r = alloc_fd();
    if (r < 0)
        return -1;
    g_fds[r].kind = K_KERNEL;
    g_fds[r].kfd  = kfds[0];

    w = alloc_fd();
    if (w < 0) {
        g_fds[r].kind = K_NONE;
        return -1;
    }
    g_fds[w].kind = K_KERNEL;
    g_fds[w].kfd  = kfds[1];

    fds[0] = r;
    fds[1] = w;
    return 0;
}

int dup2(int oldfd, int newfd)
{
    start();
    if (!valid(oldfd) || newfd < 0 || newfd >= FD_MAX)
        return -1;
    if (oldfd == newfd)
        return newfd;

    if (g_fds[newfd].kind != K_NONE)
        close(newfd);

    /* Copying the entry is the whole operation, even for a kernel object: two
     * of our numbers naming one of the kernel's is exactly what dup2 means, and
     * close() counts the references before it closes anything. */
    g_fds[newfd] = g_fds[oldfd];
    return newfd;
}

void* sbrk(long increment)
{
    return (void*)__syscall(SYS_sbrk, increment, 0, 0, 0, 0);
}
