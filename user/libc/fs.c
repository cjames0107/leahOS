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

#include <errno.h>
#include <fcntl.h>
#include <ipc.h>
#include <poll.h>
#include <shm.h>
#include <termios.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vfsd.h>

#define FD_MAX    64
#define PATH_MAX  256

#define K_NONE   0
#define K_KERNEL 1
#define K_FILE   2
/*   K_DEVICE - one of the entries in /dev. They are answered here rather than
 *   by vfsd because that is the server for a disk, and none of these is on a
 *   disk: /dev/null is a rule, and /dev/tty is a fact about *this process* -
 *   which terminal it belongs to - that a filesystem server has no way to
 *   know. Path resolution and the descriptor table already live in libc, and
 *   this is the same job.
 *
 *   The entries exist on disk too, as empty files, so that ls /dev shows them
 *   and a path that names one is not a lie. Opening one never reaches them. */
#define K_DEVICE 3

#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_FULL    3
#define DEV_TTY     4
#define DEV_CONSOLE 5

/* Where we are in an open file is not this process's business alone.
 *
 * UNIX calls the thing a descriptor points at an open file description, and
 * the position lives in it rather than in the descriptor: fork and dup2 make a
 * second reference to one description, not a second description, so the two
 * share a position. `echo one; echo two` with the shell's output in a file
 * relies on exactly that - both children start where the previous one stopped,
 * and without it the second overwrites the first.
 *
 * vfsd is addressed by path and offset and holds no per-descriptor state, so
 * the position has to be shared from here, in a page of shared memory. It
 * starts private and moves there when a second reference is about to exist -
 * see share_position. fork inherits the mapping and the pages behind it, so a
 * child needs no help; execve replaces the address space, so the id is carried
 * over and the mapping made again on first use.
 */
struct entry {
    unsigned char kind;
    int           kfd;          /* K_KERNEL: the number the kernel knows */
    int           pos_id;       /* K_FILE: the shared position, or -1 */
    long*         pos;          /* it, mapped here; null until first asked */
    long          offset;       /* the position when it could not be shared */
    unsigned      flags;
    char          path[PATH_MAX];
};

static struct entry g_fds[FD_MAX];
static char         g_cwd[PATH_MAX] = "/";
static int          g_started;

static void share_position(struct entry* e);

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
    int          tty;                   /* the controlling terminal, or -1 */
    unsigned     tty_key;               /* its control block, or 0 */
    char         cwd[PATH_MAX];
    struct entry fds[FD_MAX];
};

/* Which descriptor is the terminal this process is attached to. Not stdin:
 * stdin is redirected all the time, and `something | less` is exactly the case
 * where the two differ and where the difference matters. Set once by whoever
 * creates the terminal and inherited from there down, through fork and through
 * execve, on the same shared page as everything else here. */
static int g_tty = -1;

/* --- the terminal's control block ---------------------------------------------
 *
 * Which process group the terminal is currently listening to. A real UNIX keeps
 * this in the tty driver, where the keyboard interrupt can read it; there is no
 * tty driver here, because a terminal in this system is an ordinary program at
 * the far end of a pipe and the keyboard belongs to a window server.
 *
 * So it is a page of shared memory. The terminal creates it, everything the
 * terminal starts inherits the key through the same handover as the descriptor
 * table, tcsetpgrp writes it, and the terminal reads it when somebody presses
 * Ctrl-C. Lifetime is the terminal's, which by construction outlives every job
 * it is being asked about.
 */
struct tty_control {
    unsigned magic;
    int      foreground;        /* the process group, or 0 for none */
    /* The line settings, for the same reason and by the same route: the
     * terminal is the thing doing the line discipline, so the settings have to
     * be somewhere it can read them on every key. */
    unsigned      lflag;
    unsigned char cc[NCCS];
};

#define TTY_MAGIC 0x4C544359u   /* "LTCY" */

static unsigned            g_tty_key;
static struct tty_control* g_tty_ctl;

/* The block itself, mapped on the first call that needs it - the key travels
 * across execve but a mapping cannot. Null when this process has no terminal,
 * which is the ordinary case for everything that is not a shell. */
static struct tty_control* tty_control(void)
{
    int id;

    if (g_tty_ctl != 0)
        return g_tty_ctl;
    if (g_tty_key == 0)
        return 0;
    id = shm_open(g_tty_key, sizeof(struct tty_control), SHM_PUBLIC);
    if (id < 0)
        return 0;
    g_tty_ctl = (struct tty_control*)shm_map(id);
    if (g_tty_ctl != 0 && g_tty_ctl->magic != TTY_MAGIC) {
        /* Whoever gets here first initialises it. A fresh segment reads as
         * zero, so this is the creating terminal and nobody else. */
        g_tty_ctl->magic = TTY_MAGIC;
        g_tty_ctl->foreground = 0;
        /* What a terminal is before anybody changes it: lines, echoed, with
         * the interrupt keys doing what they say. */
        g_tty_ctl->lflag = ISIG | ICANON | ECHO;
        g_tty_ctl->cc[VINTR]  = 0x03;   /* Ctrl-C */
        g_tty_ctl->cc[VQUIT]  = 0x1C;   /* Ctrl-\ */
        g_tty_ctl->cc[VERASE] = 0x7F;
        g_tty_ctl->cc[VSUSP]  = 0x1A;   /* Ctrl-Z */
        g_tty_ctl->cc[VMIN]   = 1;
        g_tty_ctl->cc[VTIME]  = 0;
    }
    return g_tty_ctl;
}

static void start(void)
{
    int id, fd;
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
        g_tty = s->tty;
        g_tty_key = s->tty_key;
        g_tty_ctl = 0;          /* the key survives an exec; a mapping cannot */
        /* The positions are still where they were - in shared memory the last
         * image mapped. This one has a new address space and has mapped none
         * of them yet, so the pointers it just copied mean nothing here. */
        for (fd = 0; fd < FD_MAX; ++fd)
            g_fds[fd].pos = 0;
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

/* Called from fork, before the kernel is asked for a second process.
 *
 * What execve leaves behind is keyed by the pid that left it, and start() is
 * lazy - it runs on the first call that needs a descriptor. A process that
 * forks before making any such call hands the lookup to its child, where
 * getpid() answers with the child's pid, the key does not match, and the child
 * decides it is a fresh process whose descriptors are the kernel's console.
 *
 * That is how `sh -c "echo hello"` with its output redirected printed nothing
 * anywhere: sh touched no file before forking, so echo inherited a table that
 * said standard output was the console rather than the file. Commands with a
 * pipe or a pattern in them worked, because pipe() and readdir() had already
 * forced the table into existence.
 *
 * Claiming it here, while getpid() still returns the pid it was saved under,
 * is the whole fix.
 */
void __fd_before_fork(void)
{
    int fd;

    start();
    /* Everything open right now is about to be named by two processes, so its
     * position has to stop being private. Files opened after this are one
     * process's own again, until that process forks. */
    for (fd = 0; fd < FD_MAX; ++fd)
        share_position(&g_fds[fd]);
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
    s->tty = g_tty;
    s->tty_key = g_tty_key;
    s->magic = SAVED_MAGIC;
}

/* A vfsd reply is a count when it succeeded and a negated error code when it
 * did not. This is the single place that translates, so that no caller has to
 * decide what -13 meant. Anything outside the range of real codes is a
 * failure whose reason was never recorded, and EIO is the honest answer for
 * that rather than a guess. */
/* The same translation for what the kernel returns from a pipe or the console.
 * It uses the negated-errno convention too, so that an interrupted read can be
 * told apart from a closed one - and it has to be, because a shell treats the
 * second as the end of its input and exits on it. */
static long from_kernel(long answer)
{
    if (answer >= 0)
        return answer;
    errno = (answer > -ERRNO_MAX) ? (int)-answer : EIO;
    return -1;
}

static long from_vfs(long answer)
{
    if (answer >= 0)
        return answer;
    errno = (answer > -ERRNO_MAX) ? (int)-answer : EIO;
    return -1;
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

/* The key of a shared position. The pid keeps two processes apart and the
 * sequence keeps one process's own files apart; 4096 of them before the
 * sequence comes round again, which is more than anything here opens while
 * still holding the first. */
#define POS_KEY(pid, seq) \
    (0xE0000000u | (((unsigned)(pid) & 0xFFFFu) << 12) | ((unsigned)(seq) & 0xFFFu))

static unsigned g_pos_seq;

/* The position of an open file, wherever it actually lives. Mapping is done
 * here rather than at open() because execve carries the id across and leaves
 * the pointer behind: the first read or write in the new image finds it. */
static long* position(struct entry* e)
{
    if (e->pos != 0)
        return e->pos;
    if (e->pos_id >= 0) {
        long* p = (long*)shm_map(e->pos_id);
        if (p != 0) {
            e->pos = p;
            return p;
        }
        e->pos_id = -1;
    }
    /* No shared page to be had. The position still works; it just stops being
     * visible to anyone this process forks. */
    e->pos = &e->offset;
    return e->pos;
}

/* Move a position somewhere a second reference can reach it.
 *
 * Not done at open(). A position that only one descriptor in one process names
 * cannot be observed to be shared, and a segment costs a page and one of the
 * system's two hundred and fifty-six - which a program that opens files in a
 * loop would spend for nothing. The two operations that make a second
 * reference are fork and dup, so those are where the cost belongs.
 */
static void share_position(struct entry* e)
{
    long here, *there;
    int id;

    if (e->kind != K_FILE || e->pos_id >= 0)
        return;
    here = *position(e);
    id = shm_open(POS_KEY(getpid(), g_pos_seq++), sizeof(long), SHM_PUBLIC);
    if (id < 0)
        return;                 /* the private one still works, just not shared */
    there = (long*)shm_map(id);
    if (there == 0) {
        shm_destroy(id);
        return;
    }
    *there = here;
    e->pos_id = id;
    e->pos = there;
}

/* A shared position is not freed when the last descriptor here lets go of it.
 *
 * It cannot be: the whole point of sharing is that another process has it too,
 * and this side can only count its own descriptors. Closing on that count is
 * what broke pipelines - `echo one | wc -l` with the shell's output in a file
 * has the echo stage dup2 the pipe over its copy of standard output, which
 * closes the last descriptor *it* can see and destroyed a position the shell
 * and wc were still using. shm::destroy frees the slot at once, so the id went
 * straight back out to the next shm_open and two unrelated things wrote
 * through one segment; what it landed on was vfsd's transfer buffer, and the
 * next unlink never came back.
 *
 * So the lifetime is the creating process's, reclaimed when it exits - the
 * same rule the vfs transfer buffer above already lives by. That holds a slot
 * per shared description until then, out of the system's two hundred and
 * fifty-six, which is why sharing is not done at open().
 */

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

/* Which device a resolved path names, or 0 for none.
 *
 * The answer comes from the inode rather than from the path. It used to come
 * from a list of five strings here, which meant /dev/null was a rule in the C
 * library instead of a file: a second name for it did nothing, `mknod` could
 * not have worked, and a file that happened to be called /dev/null on someone
 * else's disk would have been swallowed by the rule.
 *
 * Now the node carries a number, this is a lookup of that number - the same
 * major:minor pairs Linux uses, so a device node copied from one of these
 * systems to the other means the same thing.
 */
static int device_of(unsigned rdev)
{
    switch (rdev) {
    case makedev(1, 3): return DEV_NULL;
    case makedev(1, 5): return DEV_ZERO;
    case makedev(1, 7): return DEV_FULL;
    case makedev(5, 0): return DEV_TTY;
    case makedev(5, 1): return DEV_CONSOLE;
    default:            return 0;
    }
}

int mknod(const char* path, unsigned type, unsigned mode, unsigned rdev)
{
    char resolved[PATH_MAX];
    struct ipc_message a;

    start();
    if (type != S_IFCHR && type != S_IFBLK) {
        errno = EINVAL;
        return -1;
    }
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_MKNOD, resolved,
                 type == S_IFBLK ? VFS_KIND_BLK : VFS_KIND_CHR,
                 (long)rdev, &a) != 0) {
        errno = EIO;
        return -1;
    }
    if (from_vfs(a.word[0]) < 0)
        return -1;
    if (mode != 0)
        chmod(resolved, mode);
    return 0;
}

int tty_fd(void)
{
    start();
    return g_tty;
}

void tty_set(int fd)
{
    start();
    g_tty = fd;
}

/* Whether this descriptor is the terminal.
 *
 * Not "does this process have a terminal", which is what tty_fd answers and
 * is a different question with a different use. `man ls` should page and
 * `man ls | head` should not, and both run in a process with a terminal - the
 * difference is entirely in where descriptor 1 goes. Asking the wrong one of
 * these left a pager sitting in the foreground of a pipeline whose reader had
 * already finished, holding the terminal against the person trying to type.
 */
/* Waiting on several descriptors at once.
 *
 * Split in two, because only half of this is the kernel's business. A file on
 * a disk is always ready - there is nothing to wait for and vfsd answers
 * immediately - so those are settled here without a syscall, and only the
 * pipes and the keyboard go down. If anything was ready on this side there is
 * nothing to wait for at all, and the kernel is not asked even about the ones
 * that could have blocked.
 */
int poll(struct pollfd* fds, unsigned long count, int timeout_ms)
{
    /* The kernel's own numbers, and which entry each came from. */
    struct { int fd; short events; short revents; } down[64];
    int index[64];
    unsigned long n = 0;
    int ready = 0;

    start();
    if (count > 64) {
        errno = EINVAL;
        return -1;
    }

    for (unsigned long i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (!valid(fds[i].fd)) {
            fds[i].revents = POLLNVAL;
            ++ready;
            continue;
        }
        switch (g_fds[fds[i].fd].kind) {
        case K_KERNEL:
            down[n].fd = g_fds[fds[i].fd].kfd;
            down[n].events = fds[i].events;
            down[n].revents = 0;
            index[n] = (int)i;
            ++n;
            break;
        default:
            /* A file, or one of the /dev entries. Nothing about either can
             * make a caller wait, so both directions are ready now. */
            fds[i].revents = (short)(fds[i].events & (POLLIN | POLLOUT));
            if (fds[i].revents != 0)
                ++ready;
            break;
        }
    }

    /* Something is ready already, so nothing may block - not even the ones
     * that could have. This is what makes a mixed set behave. */
    if (ready > 0)
        timeout_ms = 0;

    if (n > 0) {
        const long r = __syscall(SYS_poll, (long)down, (long)n,
                                 timeout_ms, 0, 0);
        if (r == -2) {
            errno = EINTR;
            return -1;
        }
        if (r < 0) {
            errno = EINVAL;
            return -1;
        }
        for (unsigned long i = 0; i < n; ++i)
            if (down[i].revents != 0) {
                fds[index[i]].revents = down[i].revents;
                ++ready;
            }
    } else if (ready == 0 && timeout_ms > 0) {
        /* Nothing the kernel could watch, and nothing ready: the only honest
         * thing left is to wait out the clock. */
        msleep((unsigned long)timeout_ms);
    }

    return ready;
}

int isatty(int fd)
{
    start();
    if (!valid(fd) || g_tty < 0 || !valid(g_tty))
        return 0;
    /* The same kernel object, whatever numbers the two go by here: a terminal
     * that has been dup'd is still the terminal. */
    return g_fds[fd].kind == K_KERNEL &&
           g_fds[g_tty].kind == K_KERNEL &&
           g_fds[fd].kfd == g_fds[g_tty].kfd;
}

unsigned tty_control_key(void)
{
    start();
    return g_tty_key;
}

void tty_set_control(unsigned key)
{
    start();
    g_tty_key = key;
    g_tty_ctl = 0;              /* a new key means the old mapping is not it */
}

/* Make one, for a terminal that is about to start a shell. Keyed by the
 * terminal's pid so two terminals never share a foreground group, and owned by
 * it so the block goes away when the window does. */
unsigned tty_control_create(void)
{
    start();
    g_tty_key = 0x54430000u | ((unsigned)getpid() & 0xFFFFu);
    g_tty_ctl = 0;
    tty_control();
    return g_tty_key;
}

int tcgetattr(int fd, struct termios* out)
{
    struct tty_control* c;
    (void)fd;
    start();
    c = tty_control();
    if (c == 0) {
        errno = ENOTTY;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->c_lflag = c->lflag;
    memcpy(out->c_cc, c->cc, NCCS);
    return 0;
}

int tcsetattr(int fd, int when, const struct termios* in)
{
    struct tty_control* c;
    (void)fd;
    (void)when;     /* nothing is buffered on this side to drain or flush */
    start();
    c = tty_control();
    if (c == 0) {
        errno = ENOTTY;
        return -1;
    }
    c->lflag = in->c_lflag;
    memcpy(c->cc, in->c_cc, NCCS);
    return 0;
}

void cfmakeraw(struct termios* t)
{
    t->c_iflag = 0;
    t->c_oflag = 0;
    t->c_lflag = 0;     /* no lines, no echo, no signals - all three at once */
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

pid_t tcgetpgrp(int fd)
{
    struct tty_control* c;
    (void)fd;       /* there is only ever the one terminal per process here */
    start();
    c = tty_control();
    if (c == 0) {
        errno = ENOTTY;
        return -1;
    }
    return (pid_t)c->foreground;
}

int tcsetpgrp(int fd, pid_t pgid)
{
    struct tty_control* c;
    (void)fd;
    start();
    c = tty_control();
    if (c == 0) {
        errno = ENOTTY;
        return -1;
    }
    c->foreground = (int)pgid;
    return 0;
}

int open(const char* path, int flags)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    int fd, exists, is_dir = 0, device;
    long size = 0;

    start();
    __fd_resolve(path, resolved);

    exists = vfs_call(VFS_STAT, resolved, 0, 0, &a) == 0 && a.word[0] >= 0;
    if (exists) {
        size   = a.word[0];
        is_dir = a.word[1] == VFS_KIND_DIR;
    }

    /* A character device, and one this library knows how to be. The stat above
     * is the only lookup: the device number rides back in it, so recognising
     * /dev/null costs nothing that opening any other file did not already. */
    device = 0;
    if (exists && a.word[1] == VFS_KIND_CHR && a.bytes >= 20) {
        unsigned rdev;
        memcpy(&rdev, a.data + 16, sizeof(rdev));
        device = device_of(rdev);
    }
    if (device != 0) {
        /* /dev/tty is the one that can fail: a process with no terminal has
         * none to open, and saying so lets the caller do something else. */
        if (device == DEV_TTY && g_tty < 0)
            return -1;
        fd = alloc_fd();
        if (fd < 0)
            return -1;
        g_fds[fd].kind  = K_DEVICE;
        g_fds[fd].kfd   = device;
        g_fds[fd].flags = (unsigned)flags;
        g_fds[fd].pos_id = -1;
        g_fds[fd].pos    = 0;
        g_fds[fd].offset = 0;
        return fd;
    }

    /* A FIFO is not read from the disk at all. The file on disk is a name, a
     * mode and an owner; what flows between the two ends is a kernel pipe,
     * and both ends find it by the inode number - the one identifier for this
     * file that is unique and that both sides already agree on.
     *
     * The open blocks until the other end arrives, which is the whole point:
     * without it a writer would finish before there was anybody to write to. */
    if (exists && a.word[1] == VFS_KIND_FIFO) {
        unsigned ino = 0;
        if (a.bytes >= 16)
            memcpy(&ino, a.data + 12, sizeof(ino));
        if (ino == 0) {
            errno = EIO;
            return -1;
        }
        fd = alloc_fd();
        if (fd < 0) {
            errno = EMFILE;
            return -1;
        }
        const long kfd = __syscall(SYS_openfifo, (long)ino,
                                   (flags & O_WRONLY) != 0,
                                   (flags & O_NONBLOCK) != 0, 0, 0);
        if (kfd < 0) {
            errno = kfd == -4 ? EINTR : EIO;
            return -1;
        }
        g_fds[fd].kind   = K_KERNEL;
        g_fds[fd].kfd    = (int)kfd;
        g_fds[fd].flags  = (unsigned)flags;
        g_fds[fd].pos_id = -1;
        g_fds[fd].pos    = 0;
        g_fds[fd].offset = 0;
        return fd;
    }

    if (!exists) {
        if ((flags & O_CREAT) == 0) {
            errno = ENOENT;
            return -1;
        }
        if (vfs_call(VFS_CREATE, resolved, 0, 0, &a) != 0) {
            errno = EIO;
            return -1;
        }
        if (from_vfs(a.word[0]) < 0)
            return -1;
        size = 0;
    } else if (is_dir && (flags & O_WRONLY) != 0) {
        errno = EISDIR;
        return -1;
    } else if (!permitted_stat((unsigned)a.word[2],
                          (unsigned)((a.word[3] >> 16) & 0xFFFF),
                          (unsigned)(a.word[3] & 0xFFFF),
                          (flags & O_WRONLY) != 0)) {
        /* Advisory, and deliberately so. This is the process asking itself,
         * which decides nothing - vfsd checks every read and write on the far
         * side of the boundary. It is here because open() answering "yes" and
         * the first read answering "no" is a worse interface than open()
         * saying so, and because that is what open() has always done. */
        errno = EACCES;
        return -1;
    }

    if ((flags & O_TRUNC) != 0 && exists && !is_dir && size > 0)
        vfs_call(VFS_TRUNC, resolved, 0, 0, &a);

    fd = alloc_fd();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }

    g_fds[fd].kind   = K_FILE;
    g_fds[fd].flags  = (unsigned)flags;
    g_fds[fd].pos_id = -1;
    g_fds[fd].pos    = 0;
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
    g_fds[fd].kind   = K_NONE;
    g_fds[fd].pos_id = -1;
    g_fds[fd].pos    = 0;
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
        return from_kernel(__syscall(SYS_read, e->kfd, (long)buffer,
                                     (long)count, 0, 0));
    if (e->kind == K_DEVICE) {
        switch (e->kfd) {
        case DEV_NULL:
        case DEV_FULL:
            return 0;                   /* immediately at the end */
        case DEV_ZERO:
            memset(buffer, 0, count);
            return (long)count;
        case DEV_TTY:
            /* Forwarded rather than duplicated: the terminal is a descriptor
             * this process already holds, and reading it here is reading it. */
            return (g_tty >= 0) ? read(g_tty, buffer, count) : -1;
        default:
            return __syscall(SYS_read, 0, (long)buffer, (long)count, 0, 0);
        }
    }

    while ((unsigned long)done < count) {
        unsigned long want = count - (unsigned long)done;
        long got;
        if (want > VFS_CHUNK)
            want = VFS_CHUNK;
        if (vfs_call(VFS_READ, e->path, *position(e) + done, (long)want, &a) != 0) {
            if (done > 0)
                break;
            errno = EIO;
            return -1;
        }
        got = a.word[0];
        if (got < 0) {
            if (done > 0)
                break;                  /* what was read still counts */
            return from_vfs(got);
        }
        if (got == 0)
            break;                      /* the end of the file */
        memcpy((char*)buffer + done, g_buf->data, (unsigned long)got);
        done += got;
        if ((unsigned long)got < want)
            break;
    }
    *position(e) += done;
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
        return from_kernel(__syscall(SYS_write, e->kfd, (long)buffer,
                                     (long)count, 0, 0));
    if (e->kind == K_DEVICE) {
        switch (e->kfd) {
        case DEV_NULL:
        case DEV_ZERO:
            return (long)count;         /* accepted and discarded */
        case DEV_FULL:
            return -1;                  /* the point of it: always full */
        case DEV_TTY:
            return (g_tty >= 0) ? write(g_tty, buffer, count) : -1;
        default:
            return __syscall(SYS_write, 1, (long)buffer, (long)count, 0, 0);
        }
    }

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
        if (vfs_call(VFS_WRITE, e->path, *position(e) + done, (long)want, &a) != 0) {
            if (done > 0)
                break;
            errno = EIO;
            return -1;
        }
        put = a.word[0];
        if (put < 0) {
            if (done > 0)
                break;
            return from_vfs(put);
        }
        if (put == 0)
            break;
        done += put;
    }
    *position(e) += done;
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

    {
        long* at = position(e);
        if (whence == SEEK_SET)
            *at = offset;
        else if (whence == SEEK_CUR)
            *at += offset;
        else if (whence == SEEK_END) {
            if (vfs_call(VFS_STAT, e->path, 0, 0, &a) != 0 || a.word[0] < 0)
                return -1;
            *at = a.word[0] + offset;
        } else
            return -1;

        if (*at < 0)
            *at = 0;
        return *at;
    }
}

/* stat and lstat differ by one tag, so they are one function. Which of the two
 * a caller wants is the question "am I asking about this name, or about what
 * it leads to?", and only the caller knows. */
static int stat_either(const char* path, struct stat* out, unsigned tag)
{
    char resolved[PATH_MAX];
    struct ipc_message a;

    start();
    __fd_resolve(path, resolved);
    if (vfs_call(tag, resolved, 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    if (from_vfs(a.word[0]) < 0)
        return -1;

    out->st_size = (uint64_t)a.word[0];
    out->st_type = a.word[1] == VFS_KIND_DIR  ? S_IFDIR
                 : a.word[1] == VFS_KIND_LINK ? S_IFLNK
                 : a.word[1] == VFS_KIND_FIFO ? S_IFIFO
                 : a.word[1] == VFS_KIND_CHR  ? S_IFCHR
                 : a.word[1] == VFS_KIND_BLK  ? S_IFBLK
                                              : S_IFREG;
    out->st_mode = (uint32_t)a.word[2];
    out->st_uid  = (uint32_t)((a.word[3] >> 16) & 0xFFFF);
    out->st_gid  = (uint32_t)(a.word[3] & 0xFFFF);
    /* The three timestamps ride in the data, which stat has no other use for.
     * An older vfsd that sends none leaves them at zero, which reads as 1970 -
     * wrong in a way that is obvious rather than plausible. */
    out->st_mtime = out->st_ctime = out->st_atime = 0;
    out->st_ino = 0;
    if (a.bytes >= 12) {
        unsigned stamps[3];
        memcpy(stamps, a.data, sizeof(stamps));
        out->st_mtime = (int64_t)stamps[0];
        out->st_ctime = (int64_t)stamps[1];
        out->st_atime = (int64_t)stamps[2];
    }
    if (a.bytes >= 16) {
        unsigned ino;
        memcpy(&ino, a.data + 12, sizeof(ino));
        out->st_ino = ino;
    }
    out->st_rdev = 0;
    if (a.bytes >= 20) {
        unsigned rdev;
        memcpy(&rdev, a.data + 16, sizeof(rdev));
        out->st_rdev = rdev;
    }
    return 0;
}

int stat(const char* path, struct stat* out)
{
    return stat_either(path, out, VFS_STAT);
}

int lstat(const char* path, struct stat* out)
{
    return stat_either(path, out, VFS_LSTAT);
}

/* Two strings in the one data field. Both symlink and link take a pair, and
 * in both the first is the thing being pointed at - for a symlink that is text
 * stored as written, for a hard link it is a path resolved now. */
static int vfs_pair(unsigned tag, const char* first, const char* second)
{
    struct ipc_message q, a;
    unsigned n = 0, k = 0;

    if (vfs_port() < 0)
        return -1;
    memset(&q, 0, sizeof(q));
    q.tag = tag;
    while (first[n] != '\0' && n + 2 < sizeof(q.data))
        { q.data[n] = first[n]; ++n; }
    q.data[n++] = '\0';
    while (second[k] != '\0' && n + 1 < sizeof(q.data))
        { q.data[n++] = second[k++]; }
    q.data[n++] = '\0';
    q.bytes = n;
    q.shm_key = (int)g_buf_key;
    q.shm_bytes = sizeof(struct vfs_shared);

    memset(&a, 0, sizeof(a));
    if (ipc_call(g_vfs, &q, &a) != 0) {
        errno = EIO;
        return -1;
    }
    return from_vfs(a.word[0]) < 0 ? -1 : 0;
}

int symlink(const char* target, const char* path)
{
    char resolved[PATH_MAX];
    start();
    __fd_resolve(path, resolved);
    /* The target is not resolved: it is stored exactly as written, and may be
     * relative, or name something that does not exist yet or ever. */
    return vfs_pair(VFS_SYMLINK, target, resolved);
}

int mkfifo(const char* path, unsigned mode)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    (void)mode;     /* one mode for now: 0644, as create uses */

    start();
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_MKFIFO, resolved, 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    return from_vfs(a.word[0]) < 0 ? -1 : 0;
}

int link(const char* existing, const char* path)
{
    char from[PATH_MAX], to[PATH_MAX];
    start();
    __fd_resolve(existing, from);
    __fd_resolve(path, to);
    return vfs_pair(VFS_LINK, from, to);
}

long readlink(const char* path, char* out, unsigned long max)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    unsigned long n;

    start();
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_READLINK, resolved, 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    if (from_vfs(a.word[0]) < 0)
        return -1;

    /* No terminating null, as everywhere: a target may contain anything, so
     * the length is the answer and the caller adds the null if it wants one. */
    n = (unsigned long)a.word[0];
    if (n > max)
        n = max;
    memcpy(out, a.data, n);
    return (long)n;
}

long fsck(int repair, char* report, unsigned long max, unsigned* fixed)
{
    struct ipc_message a;

    start();
    if (report != 0 && max > 0)
        report[0] = '\0';
    if (fixed != 0)
        *fixed = 0;
    if (vfs_call(VFS_FSCK, "", repair != 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    if (report != 0 && max > 0) {
        unsigned long n = a.bytes;
        if (n > max - 1)
            n = max - 1;
        memcpy(report, a.data, n);
        report[n] = '\0';
    }
    if (fixed != 0)
        *fixed = (unsigned)a.word[1];
    if (from_vfs(a.word[0]) < 0)
        return -1;
    return a.word[0];
}

int statfs(const char* path, struct statfs* out)
{
    char resolved[PATH_MAX];
    struct ipc_message a;

    start();
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_STATFS, resolved, 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    out->f_bsize  = (uint64_t)a.word[0];
    out->f_blocks = (uint64_t)a.word[1];
    out->f_bfree  = (uint64_t)a.word[2];
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
        buffer[n].d_type = a.word[0] == VFS_KIND_DIR  ? S_IFDIR
                         : a.word[0] == VFS_KIND_LINK ? S_IFLNK
                         : a.word[0] == VFS_KIND_FIFO ? S_IFIFO
                         : a.word[0] == VFS_KIND_CHR  ? S_IFCHR
                         : a.word[0] == VFS_KIND_BLK  ? S_IFBLK
                                                      : S_IFREG;
        buffer[n].d_size = (uint64_t)a.word[1];
        buffer[n].d_mode = (unsigned)a.word[2];
        buffer[n].d_mtime = a.word[3];
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
    if (vfs_call(VFS_MKDIR, resolved, 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    return from_vfs(a.word[0]) < 0 ? -1 : 0;
}

int unlink(const char* path)
{
    char resolved[PATH_MAX];
    struct ipc_message a;
    start();
    __fd_resolve(path, resolved);
    if (vfs_call(VFS_UNLINK, resolved, 0, 0, &a) != 0) {
        errno = EIO;
        return -1;
    }
    return from_vfs(a.word[0]) < 0 ? -1 : 0;
}

int rename(const char* oldpath, const char* newpath)
{
    char from[PATH_MAX], to[PATH_MAX];
    struct ipc_message q, a;
    unsigned at = 0, i;

    start();
    if (vfs_port() < 0)
        return -1;
    __fd_resolve(oldpath, from);
    __fd_resolve(newpath, to);

    /* Two paths in one message, packed as consecutive strings - the only call
     * here that names two files. */
    memset(&q, 0, sizeof(q));
    q.tag = VFS_RENAME;
    q.shm_key = (int)g_buf_key;
    q.shm_bytes = sizeof(struct vfs_shared);
    for (i = 0; from[i] != '\0' && at + 2 < sizeof(q.data); ++i)
        q.data[at++] = from[i];
    q.data[at++] = '\0';
    for (i = 0; to[i] != '\0' && at + 2 < sizeof(q.data); ++i)
        q.data[at++] = to[i];
    q.data[at++] = '\0';
    q.bytes = at;

    memset(&a, 0, sizeof(a));
    return ipc_call(g_vfs, &q, &a) == 0 && a.word[0] >= 0 ? 0 : -1;
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
     * close() counts the references before it closes anything.
     *
     * The position goes shared first. Two descriptors for one open file share
     * where they are - `cmd >f 2>&1` is the everyday case - and a copy of a
     * private position would give them one each. */
    share_position(&g_fds[oldfd]);
    g_fds[newfd] = g_fds[oldfd];
    return newfd;
}

void* sbrk(long increment)
{
    return (void*)__syscall(SYS_sbrk, increment, 0, 0, 0, 0);
}

/* The lowest free descriptor pointing at the same thing, which is what dup has
 * always meant. Used to give a terminal a second number that redirection will
 * not move. */
int dup(int oldfd)
{
    start();
    if (!valid(oldfd))
        return -1;
    const int fd = alloc_fd();
    if (fd < 0)
        return -1;
    /* The same second reference dup2 makes, so the same sharing: two numbers
     * for one open file are two numbers for one position. */
    share_position(&g_fds[oldfd]);
    g_fds[fd] = g_fds[oldfd];
    return fd;
}
