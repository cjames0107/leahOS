/* leahOS self-tests for the userland-facing kernel features.
 *
 * Run it from the shell as `tests`. Each check prints a pass/fail line, and the
 * exit status is the number of failures, so it doubles as something a script
 * can act on.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <thread.h>
#include <unistd.h>

static int g_failures;

static void check(const char* what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static void test_mmap(void)
{
    printf("mmap:\n");

    /* A plain anonymous mapping, big enough to span several pages. */
    const size_t len = 3 * 4096 + 100;
    char* p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap returns a mapping", p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;

    /* Fresh anonymous memory must read back as zero. */
    int zeroed = 1;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] != 0)
            zeroed = 0;
    }
    check("new pages are zero filled", zeroed);

    /* Write the whole span, including the last page, then read it back. */
    for (size_t i = 0; i < len; ++i)
        p[i] = (char)(i * 7 + 3);
    int kept = 1;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] != (char)(i * 7 + 3))
            kept = 0;
    }
    check("writes survive across pages", kept);

    /* A second mapping must not overlap the first. */
    char* q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("a second mapping is distinct", q != MAP_FAILED && (q + 4096 <= p || q >= p + len));

    check("munmap succeeds", munmap(p, len) == 0);
    if (q != MAP_FAILED)
        munmap(q, 4096);
}

/* Shared across threads: proving they really share one address space. */
static volatile int g_counter;
static volatile int g_seen_tids[4];

static void worker(void* arg)
{
    int slot = (int)(long)arg;
    g_seen_tids[slot] = gettid();
    for (int i = 0; i < 1000; ++i)
        g_counter = g_counter + 1;
}

static void test_threads(void)
{
    printf("threads:\n");

    const int self = gettid();
    check("gettid and getpid agree on the main thread", self == getpid());

    g_counter = 0;
    tid_t made[4];
    int all_started = 1;
    for (int i = 0; i < 4; ++i) {
        made[i] = thread_create(worker, (void*)(long)i);
        if (made[i] < 0)
            all_started = 0;
    }
    check("four threads start", all_started);

    for (int i = 0; i < 4; ++i)
        thread_join();

    /* Every thread wrote into the same globals, so the parent sees their work:
     * that is the whole point of sharing an address space rather than forking. */
    check("threads share the parent's memory", g_counter == 4000);

    int distinct = 1;
    for (int i = 0; i < 4; ++i) {
        if (g_seen_tids[i] == self || g_seen_tids[i] == 0)
            distinct = 0;
        for (int j = i + 1; j < 4; ++j) {
            if (g_seen_tids[i] == g_seen_tids[j])
                distinct = 0;
        }
    }
    check("each thread has its own tid", distinct);

    /* A thread is not a process: they all report the same getpid(). */
    check("the process survived its threads exiting", getpid() == self);
}

/* A deliberately racy counter: read, yield, write back. Without a lock the
 * interleaving loses updates; with one the total is exact. */
static mutex_t g_lock = MUTEX_INIT;
static volatile int g_guarded;
static volatile int g_unguarded;

static void locker(void* arg)
{
    (void)arg;
    for (int i = 0; i < 200; ++i) {
        mutex_lock(&g_lock);
        int value = g_guarded;
        yield();              /* widen the window a real race would need */
        g_guarded = value + 1;
        mutex_unlock(&g_lock);
    }
}

static void racer(void* arg)
{
    (void)arg;
    for (int i = 0; i < 200; ++i) {
        int value = g_unguarded;
        yield();
        g_unguarded = value + 1;
    }
}

static void test_mutex(void)
{
    printf("thread synchronisation:\n");

    check("an uncontended lock is taken", mutex_trylock(&g_lock) == 0);
    check("a held lock refuses trylock", mutex_trylock(&g_lock) == -1);
    mutex_unlock(&g_lock);
    check("trylock succeeds again after unlock", mutex_trylock(&g_lock) == 0);
    mutex_unlock(&g_lock);

    g_guarded = 0;
    for (int i = 0; i < 4; ++i)
        thread_create(locker, 0);
    for (int i = 0; i < 4; ++i)
        thread_join();
    check("a mutex serialises 4 threads over a shared counter", g_guarded == 800);

    /* The control: the same code without the lock should lose updates. If this
     * ever came out exact, the test above would be proving nothing. */
    g_unguarded = 0;
    for (int i = 0; i < 4; ++i)
        thread_create(racer, 0);
    for (int i = 0; i < 4; ++i)
        thread_join();
    check("the same loop without a lock does race", g_unguarded < 800);
}

static volatile int g_caught;
static volatile int g_caught_signo;

static void catcher(int signo)
{
    g_caught_signo = signo;
    ++g_caught;
}

static void test_signals(void)
{
    printf("signals:\n");

    g_caught = 0;
    g_caught_signo = 0;
    check("signal() accepts a handler", signal(SIGUSR1, catcher) != SIG_ERR);

    /* Delivery happens on the way out of a syscall, so raise() itself is the
     * syscall that carries it. */
    raise(SIGUSR1);
    check("a raised signal runs the handler", g_caught == 1);
    check("the handler sees the right number", g_caught_signo == SIGUSR1);

    /* The interesting half: after the handler returns through the restorer,
     * sigreturn must put the interrupted context back exactly. Local state
     * either side of the signal is what proves it. */
    volatile int before = 0x1234;
    raise(SIGUSR1);
    volatile int after = before + 1;
    check("execution resumes correctly after a handler", after == 0x1235);
    check("the handler ran a second time", g_caught == 2);

    /* Ignored signals are dropped rather than delivered. */
    signal(SIGUSR2, SIG_IGN);
    const int was = g_caught;
    raise(SIGUSR2);
    check("an ignored signal is not delivered", g_caught == was);

    /* Reset to default; sending it now would kill us, so only check the
     * bookkeeping - that setting a disposition returns the previous one. */
    check("signal() returns the previous handler",
          signal(SIGUSR1, SIG_DFL) == catcher);

    /* A signal that kills: the child takes the default action and dies with
     * 128 + signo, the convention for death by signal. */
    int pid = fork();
    if (pid == 0) {
        for (;;)
            getpid();          /* a syscall, so delivery has somewhere to land */
    }
    kill(pid, SIGTERM);
    int status = 0;
    wait(&status);
    check("SIGTERM kills a child by default", status == 128 + SIGTERM);

    /* SIGKILL cannot be caught, however hard a process tries. */
    pid = fork();
    if (pid == 0) {
        signal(SIGKILL, catcher);
        for (;;)
            getpid();
    }
    kill(pid, SIGKILL);
    status = 0;
    wait(&status);
    check("SIGKILL cannot be caught", status == 128 + SIGKILL);
}

static void test_permissions(void)
{
    printf("users and permissions:\n");

    check("we start as root", getuid() == 0);

    /* A file created by root is owned by root, with the default mode. */
    const int fd = open("/perm.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, "secret\n", 7);
        close(fd);
    }
    struct stat st;
    check("a new file is owned by its creator", stat("/perm.txt", &st) == 0 && st.st_uid == 0);
    check("a new file is mode 0644", (st.st_mode & 0777) == 0644);

    check("chmod changes the mode", chmod("/perm.txt", 0600) == 0);
    check("the new mode is stored on disk",
          stat("/perm.txt", &st) == 0 && (st.st_mode & 0777) == 0600);

    /* The enforcement test has to happen in a child, because dropping root is
     * a one-way door: a non-root process cannot become root again. */
    int pid = fork();
    if (pid == 0) {
        int failures = 0;

        if (setuid(1000) != 0)
            failures |= 1;
        if (getuid() != 1000)
            failures |= 2;
        /* Mode 0600 owned by root: another user must not get in. */
        if (open("/perm.txt", O_RDONLY) >= 0)
            failures |= 4;
        /* Nor may they chmod a file they do not own... */
        if (chmod("/perm.txt", 0666) == 0)
            failures |= 8;
        /* ...nor give a file away. */
        if (chown("/perm.txt", 1000, (unsigned)-1) == 0)
            failures |= 16;
        /* And having dropped root, they cannot climb back. */
        if (setuid(0) == 0)
            failures |= 32;
        exit(failures);
    }
    int status = 0;
    wait(&status);
    check("setuid drops to another user", (status & 1) == 0 && (status & 2) == 0);
    check("a non-owner cannot open a 0600 file", (status & 4) == 0);
    check("a non-owner cannot chmod", (status & 8) == 0);
    check("a non-root user cannot chown", (status & 16) == 0);
    check("dropping root is one-way", (status & 32) == 0);

    /* Now make it world readable and confirm the same user can read it: the
     * check has to pass as well as fail, or it proves nothing. */
    chmod("/perm.txt", 0644);
    pid = fork();
    if (pid == 0) {
        setuid(1000);
        exit(open("/perm.txt", O_RDONLY) >= 0 ? 0 : 1);
    }
    status = 0;
    wait(&status);
    check("a world-readable file is readable by others", status == 0);

    /* Root ignores the mode bits entirely. */
    chmod("/perm.txt", 0000);
    check("root bypasses permission checks", open("/perm.txt", O_RDONLY) >= 0);

    unlink("/perm.txt");
}

int main(void)
{
    printf("\nleahOS self-tests\n\n");
    test_mmap();
    test_threads();
    test_mutex();
    test_signals();
    test_permissions();
    printf("\n%d failure(s)\n", g_failures);
    return g_failures;
}
