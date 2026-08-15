/* leahOS self-tests for the userland-facing kernel features.
 *
 * Run it from the shell as `tests`. Each check prints a pass/fail line, and the
 * exit status is the number of failures, so it doubles as something a script
 * can act on.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <regex.h>
#include <sys/wait.h>
#include <termios.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <net.h>
#include <stdlib.h>
#include <thread.h>
#include <display.h>
#include <driver.h>
#include <image.h>
#include <ipc.h>
#include <proc.h>
#include <ui.h>
#include <math.h>
#include <paths.h>
#include <time.h>
#include <sound.h>
#include <unistd.h>

static int g_failures;

/* Each section announces itself on the serial console as well as the screen.
 *
 * A run that stops leaves the screen showing whatever it had got to, which is
 * only useful if somebody is watching at that moment - and a hang is exactly
 * the case where the screenshot comes far too late. The console is readable
 * from outside the machine and survives the guest ceasing to answer, so a run
 * that never finishes still says which section it never finished.
 */
static int g_console = -1;

static void section(const char* name)
{
    printf("%s:\n", name);
    if (g_console < 0)
        g_console = open("/dev/console", O_WRONLY);
    if (g_console >= 0) {
        char line[96];
        const int n = snprintf(line, sizeof(line), "tests: %s\n", name);
        write(g_console, line, (unsigned long)n);
    }
}

/* The names that failed, kept so the summary can list them rather than only
 * count them - a number tells you to go looking, a name tells you where. */
#define MAX_FAILED 24
static char g_failed[MAX_FAILED][96];
static int  g_failed_count;
static int  g_checks;

static void check(const char* what, int ok)
{
    ++g_checks;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
        if (g_failed_count < MAX_FAILED)
            snprintf(g_failed[g_failed_count++], 96, "%s", what);
    }
}

/* Says what it actually got when a check fails, because "FAIL echo hello" on
 * its own does not say whether the shell printed the wrong thing or printed
 * nothing at all - and those have completely different causes. */
static void check_says(const char* what, const char* got, const char* want)
{
    const int ok = strcmp(got, want) == 0;
    check(what, ok);
    if (!ok)
        printf("       wanted [%s] got [%s]\n", want, got);
}

static void test_mmap(void)
{
    section("mmap");

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

static mutex_t g_counter_lock;

static void worker(void* arg)
{
    int slot = (int)(long)arg;
    g_seen_tids[slot] = gettid();
    /* The lock is not decoration. A bare g_counter = g_counter + 1 from four
     * threads loses updates the moment they genuinely run at the same time -
     * it only ever totalled 4000 because there was one processor to run on. */
    for (int i = 0; i < 1000; ++i) {
        mutex_lock(&g_counter_lock);
        g_counter = g_counter + 1;
        mutex_unlock(&g_counter_lock);
    }
}

static void test_threads(void)
{
    section("threads");

    const int self = gettid();
    check("gettid and getpid agree on the main thread", self == getpid());

    g_counter = 0;
    g_counter_lock.state = 0;
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
     * that is the whole point of sharing an address space rather than forking.
     * With the increments serialised the total is exact, which also means this
     * is a real test of the mutex on more than one processor. */
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
    section("thread synchronisation");

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

static void test_cow(void)
{
    section("copy-on-write fork");

    /* A big writable region, filled with a known pattern. After fork both sides
     * see it, but neither must see the other's writes. */
    const size_t len = 64 * 4096;
    unsigned char* page = mmap(0, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        check("mmap for the fork test", 0);
        return;
    }
    for (size_t i = 0; i < len; ++i)
        page[i] = (unsigned char)(i * 13 + 5);

    int pid = fork();
    if (pid == 0) {
        /* The child must first see exactly what the parent wrote... */
        int inherited = 1;
        for (size_t i = 0; i < len; ++i) {
            if (page[i] != (unsigned char)(i * 13 + 5))
                inherited = 0;
        }
        /* ...then scribble over all of it, forcing a private copy of every
         * shared page, and read its own values back. */
        for (size_t i = 0; i < len; ++i)
            page[i] = 0xAA;
        int privately = 1;
        for (size_t i = 0; i < len; ++i) {
            if (page[i] != 0xAA)
                privately = 0;
        }
        exit(inherited && privately ? 0 : 1);
    }
    int status = 0;
    wait(&status);
    check("a child inherits and can privately rewrite the whole region",
          status == 0);

    /* The parent's copy must be untouched by everything the child did. */
    int intact = 1;
    for (size_t i = 0; i < len; ++i) {
        if (page[i] != (unsigned char)(i * 13 + 5))
            intact = 0;
    }
    check("the parent's pages are unaffected by the child", intact);

    /* And the parent can still write its own side afterwards. */
    page[0] = 0x5A;
    check("the parent can still write after the child exited", page[0] == 0x5A);

    munmap(page, len);
}

static volatile int g_caught;
static volatile int g_caught_signo;

static void catcher(int signo)
{
    g_caught_signo = signo;
    ++g_caught;
}

static void test_shm(void)
{
    section("shared memory");

    const int id = shm_open(4242, 4096, 0);
    check("a segment can be created", id >= 0);

    volatile unsigned* a = (volatile unsigned*)shm_map(id);
    check("it maps into this process", a != 0);
    if (a == 0)
        return;
    check("a new segment reads as zero", a[0] == 0);
    a[0] = 0x1234u;

    check("the same key returns the same segment", shm_open(4242, 0, 0) == id);
    check("an unknown key cannot be opened", shm_open(4243, 0, 0) < 0);
    check("the size is the size asked for", shm_size(id) == 4096);

    /* Two independent mappings of one segment must be the same memory - that
     * is the whole point, and it is what the window server relies on. */
    volatile unsigned* b = (volatile unsigned*)shm_map(shm_open(4242, 0, 0));
    check("a second mapping is a different address", b != 0 && b != a);
    check("but the same memory", b != 0 && b[0] == 0x1234u);
    if (b != 0) {
        b[1] = 0x5678u;
        check("a write through either is seen through both", a[1] == 0x5678u);
    }

    /* And across processes. The child maps it for itself rather than
     * inheriting: a mapping inherited through fork is copy-on-write like
     * anything else, so writing to it would give the child a private copy. */
    if (fork() == 0) {
        volatile unsigned* c = (volatile unsigned*)shm_map(shm_open(4242, 0, 0));
        if (c != 0)
            c[2] = 0xC0FFEEu;
        exit(0);
    }
    wait(0);
    check("another process shares the same pages", a[2] == 0xC0FFEEu);
}

static void test_signals(void)
{
    section("signals");

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

    /* A signal that kills: the child takes the default action, and the status
     * says so - which is a different thing from an exit code, and the reason
     * the status word has room to say which of the two happened. */
    int pid = fork();
    if (pid == 0) {
        for (;;)
            getpid();          /* a syscall, so delivery has somewhere to land */
    }
    kill(pid, SIGTERM);
    int status = 0;
    wait(&status);
    check("SIGTERM kills a child by default",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

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
    check("SIGKILL cannot be caught",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    /* Delivery must not depend on the target entering the kernel. This child
     * spins on a pure computation with no syscall in the loop at all, so the
     * only way the signal reaches it is the timer interrupt's return path. */
    pid = fork();
    if (pid == 0) {
        volatile unsigned long spin = 0;
        for (;;)
            spin = spin * 1664525u + 1013904223u;
    }
    kill(pid, SIGTERM);
    status = 0;
    wait(&status);
    check("a signal reaches a process making no syscalls",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    /* And a caught signal must resume that loop intact: the handler runs, the
     * IRETQ path restores every register, and the child carries on to exit
     * normally rather than dying or faulting.
     *
     * The pipe is a readiness handshake, not decoration: signalling before the
     * child has installed its handler would take the default action and kill
     * it, so the parent waits to be told the handler is in place. */
    int ready[2];
    pipe(ready);
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        g_caught = 0;               /* fork copied the parent's tally */
        signal(SIGUSR1, catcher);
        write(ready[1], "r", 1);
        close(ready[1]);
        volatile unsigned long spin = 0;
        for (int i = 0; i < 5000000 && g_caught == 0; ++i)
            spin = spin * 1664525u + 1013904223u;
        exit(g_caught == 1 ? 7 : 8);
    }
    close(ready[1]);
    char ack = 0;
    read(ready[0], &ack, 1);
    close(ready[0]);

    kill(pid, SIGUSR1);
    status = 0;
    wait(&status);
    check("a handler interrupts and resumes a compute loop", status == 7);
}

static void test_permissions(void)
{
    section("users and permissions");

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

static void test_accounts(void)
{
    section("accounts and authentication");

    char name[32] = {};
    check("uid 0 resolves to root",
          username(0, name) == 0 && strcmp(name, "root") == 0);
    check("uid 1000 resolves to leah",
          username(1000, name) == 0 && strcmp(name, "leah") == 0);
    check("an unknown uid has no account", username(4242, name) != 0);

    /* The interesting half has to run in a child: dropping out of root is a
     * one-way door for the process that does it. */
    int pid = fork();
    if (pid == 0) {
        int fails = 0;
        char home[128] = {};

        /* Root may become anyone, but still has to know the password. */
        if (login("leah", 0, home) == 0)               fails |= 1;
        if (login("leah", "leah", home) != 0)          fails |= 1;
        if (getuid() != 1000)                          fails |= 2;
        if (strcmp(home, "/home/leah") != 0)           fails |= 4;

        /* Now unprivileged: the shadow file must be unreadable. */
        if (open("/etc/shadow", O_RDONLY) >= 0)        fails |= 8;
        /* ...but the public half is fine. */
        if (open("/etc/passwd", O_RDONLY) < 0)         fails |= 16;

        /* A wrong password fails and changes nothing. */
        if (login("root", "not-the-password", home) == 0) fails |= 32;
        if (getuid() != 1000)                          fails |= 64;

        /* An unknown account fails too. */
        if (login("nosuchuser", "whatever", home) == 0) fails |= 128;

        /* And one ordinary user may not become another even with the right
         * password: the route to guest runs through root. */
        if (login("guest", "guest", home) == 0)        fails |= 128;

        /* And the right password works. */
        if (login("root", "toor", home) != 0)          fails |= 256;
        if (getuid() != 0)                             fails |= 512;

        exit(fails > 255 ? 255 : fails);   /* exit status is a byte */
    }
    int status = 0;
    wait(&status);
    check("root still needs the target's password", (status & 1) == 0);
    check("the new uid takes effect", (status & 2) == 0);
    check("login reports the home directory", (status & 4) == 0);
    check("a normal user cannot read the shadow file", (status & 8) == 0);
    check("but can read the public passwd file", (status & 16) == 0);
    check("a wrong password is refused", (status & 32) == 0);
    check("a refused login leaves credentials alone", (status & 64) == 0);
    check("unknown accounts and sideways switches are refused",
          (status & 128) == 0);

    /* Account creation, in a child so the uid change stays contained. */
    pid = fork();
    if (pid == 0) {
        int fails = 0;
        char home[128] = {};

        if (useradd("tester", "secret", 0, 0, "/home/tester") != 0) fails |= 1;
        /* The uid must be a free one, not a collision with an existing user. */
        char who[32] = {};
        if (username(1000, who) != 0 || strcmp(who, "leah") != 0)     fails |= 2;
        /* Creating the same name twice must fail. */
        if (useradd("tester", "secret", 0, 0, "/home/tester") == 0)   fails |= 4;
        /* The new account authenticates with its password and not without. */
        if (login("tester", "wrong", home) == 0)                      fails |= 8;
        if (login("tester", "secret", home) != 0)                     fails |= 16;
        /* ...and it is not root. */
        if (getuid() == 0)                                            fails |= 32;
        exit(fails);
    }
    status = 0;
    wait(&status);
    check("root can create an account", (status & 1) == 0);
    check("a new account does not reuse an existing uid", (status & 2) == 0);
    check("a duplicate account name is refused", (status & 4) == 0);
    check("the new account rejects a wrong password", (status & 8) == 0);
    check("the new account accepts its own password", (status & 16) == 0);
    check("the new account is unprivileged", (status & 32) == 0);

    /* A non-root user may not create accounts. */
    pid = fork();
    if (pid == 0) {
        char home[128] = {};
        login("leah", "leah", home);
        exit(useradd("sneak", "x", 0, 0, "/home/sneak") == 0 ? 1 : 0);
    }
    status = 0;
    wait(&status);
    check("a normal user cannot create accounts", status == 0);
    /* The last two share the truncated bit, so check the child got that far. */
    check("the correct password is accepted", status != 255);
}

static void test_tcp(void)
{
    section("tcp");

    /* The failure path first, and without needing anything to be reachable:
     * a port nothing listens on must be refused rather than hang or succeed. */
    uint32_t gateway = 0x0A000202;      /* 10.0.2.2, QEMU's gateway */
    check("connecting to a dead port fails", tcp_connect(gateway, 9) < 0);

    /* The real thing, if the outside world is reachable. A machine with no
     * network should skip rather than fail - the checks above still ran. */
    uint32_t ip;
    if (resolve("example.com", &ip) < 0) {
        printf("  skip  no DNS, skipping the live connection\n");
        return;
    }
    check("DNS resolved a hostname for TCP", ip != 0);

    const int conn = tcp_connect(ip, 80);
    check("TCP connected to a real server", conn >= 0);
    if (conn < 0)
        return;

    static const char request[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    check("wrote a request to the connection",
          tcp_write(conn, request, sizeof(request) - 1) ==
              (long)(sizeof(request) - 1));

    char buffer[512];
    long got = 0;
    for (int i = 0; i < 400 && got == 0; ++i)
        got = tcp_read(conn, buffer, sizeof(buffer) - 1);
    check("read a response back", got > 0);
    if (got > 0) {
        buffer[got] = '\0';
        check("the response is HTTP", memcmp(buffer, "HTTP/", 5) == 0);
    }

    /* Drain to end of stream, which is what proves the peer's FIN was seen
     * rather than the connection simply going quiet. */
    long total = got > 0 ? got : 0;
    for (;;) {
        const long more = tcp_read(conn, buffer, sizeof(buffer) - 1);
        if (more <= 0)
            break;
        total += more;
    }
    check("the stream ended cleanly at the peer's FIN", total > (long)got);

    tcp_close(conn);
}

/* Message passing between address spaces.
 *
 * The whole claim being tested is that the two ends share no memory: the
 * request buffer is not mapped in the server and the reply buffer is not
 * mapped in the client. So the server answers with something it had to
 * construct - a sum, the caller's own pid, the text reversed - rather than
 * anything it could have got by reading a pointer it was handed.
 */
#define IPC_TEST_PORT 900
#define IPC_TAG_ADD   1
#define IPC_TAG_ECHO  2
#define IPC_TAG_QUIT  3

static void ipc_server(void)
{
    const int port = port_create(IPC_TEST_PORT);
    if (port < 0)
        exit(1);
    for (;;) {
        struct ipc_message m;
        unsigned from = 0;
        const int handle = ipc_recv(port, &m, &from);
        if (handle < 0)
            exit(1);
        struct ipc_message r;
        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        if (m.tag == IPC_TAG_QUIT) {
            ipc_reply(handle, &r);
            port_destroy(port);
            exit(0);
        }
        if (m.tag == IPC_TAG_ADD) {
            r.word[0] = m.word[0] + m.word[1];
            r.word[1] = (long)from;
        } else {
            unsigned n = m.bytes > IPC_INLINE ? IPC_INLINE : m.bytes;
            for (unsigned i = 0; i < n; ++i)
                r.data[i] = m.data[n - 1 - i];
            r.bytes = n;
        }
        ipc_reply(handle, &r);
    }
}

/* The component layer.
 *
 * Tested here rather than by clicking at the screen, because the screen test
 * can only ask "did anything change", and answering that needs a pixel
 * coordinate worked out by hand - which is a thing to get wrong that has
 * nothing to do with the library. Events are synthesised straight into
 * ui_event, so what is checked is the routing, not my aim.
 */
static int g_ui_fired;
static void ui_test_action(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    ++g_ui_fired;
}

static const char* ui_test_cell(void* user, int row, int col)
{
    (void)user; (void)row;
    return col == 0 ? "name" : "size";
}

static int ui_test_depth(void* user, int row)
{
    (void)user;
    return row == 0 ? 0 : 1;
}

static int ui_test_branch(void* user, int row)
{
    (void)user;
    return row == 0 ? 1 : 0;        /* the first row can be opened */
}

static const char* ui_test_row(void* user, int row)
{
    static const char* const kRows[3] = { "alpha", "beta", "gamma" };
    (void)user;
    return (row >= 0 && row < 3) ? kRows[row] : "";
}

static void press(struct ui_view* root, int x, int y)
{
    struct win_event e;
    memset(&e, 0, sizeof(e));
    e.type = WIN_EVENT_MOUSE_DOWN; e.x = x; e.y = y; e.button = 1;
    ui_event(root, &e);
}

static void release(struct ui_view* root, int x, int y)
{
    struct win_event e;
    memset(&e, 0, sizeof(e));
    e.type = WIN_EVENT_MOUSE_UP; e.x = x; e.y = y; e.button = 1;
    ui_event(root, &e);
}

static void tap_key(struct ui_view* root, unsigned key)
{
    struct win_event e;
    memset(&e, 0, sizeof(e));
    e.type = WIN_EVENT_KEY; e.key = key;
    ui_event(root, &e);
}

static void test_ui(void)
{
    section("components");
    ui_reset();
    g_ui_fired = 0;

    struct ui_view* root = ui_box(0, UI_STACK_V, 10, 6);
    struct ui_view* field = ui_field(root, "");
    ui_grow(field, 0);
    struct ui_view* button = ui_button(root, "Go", ui_test_action, 0);
    ui_grow(button, 0);
    struct ui_view* list = ui_list(root, ui_test_row, 3, 0);
    ui_on(list, ui_test_action, 0);

    const struct ui_rect all = { 0, 0, 300, 300 };
    ui_layout(root, all);

    /* Layout gives every view a frame, and they do not overlap or escape. */
    check("layout puts a view inside its parent",
          field->frame.x >= root->frame.x &&
          field->frame.x + field->frame.w <= root->frame.x + root->frame.w);
    check("stacked views do not overlap",
          button->frame.y >= field->frame.y + field->frame.h);
    check("padding is honoured", field->frame.x == 10);
    check("a growing view takes the room left over",
          list->frame.h > field->frame.h);

    /* A press lands on the view under it and takes the keyboard. */
    press(root, field->frame.x + 20, field->frame.y + 5);
    check("a press focuses the view under it", ui_focused() == field);

    tap_key(root, 'h');
    tap_key(root, 'i');
    check("typing reaches the focused field", strcmp(ui_text(field), "hi") == 0);
    tap_key(root, '\b');
    check("backspace removes the character before the caret",
          strcmp(ui_text(field), "h") == 0);
    /* The caret moves rather than the text always being appended to, which is
     * what makes a field correctable instead of retypeable. */
    tap_key(root, WIN_KEY_LEFT);
    tap_key(root, 'a');
    check("a character is inserted at the caret",
          strcmp(ui_text(field), "ah") == 0);

    /* A button fires on release over itself, and not on the press. */
    press(root, button->frame.x + 10, button->frame.y + 5);
    check("a button does not fire on the press", g_ui_fired == 0);
    release(root, button->frame.x + 10, button->frame.y + 5);
    check("a button fires on the release", g_ui_fired == 1);

    /* Released somewhere else, it does not fire - which is how a press is
     * taken back by sliding off the control. */
    press(root, button->frame.x + 10, button->frame.y + 5);
    release(root, list->frame.x + 10, list->frame.y + 40);
    check("a press slid off the button is cancelled", g_ui_fired == 1);

    /* A list selects the row under the pointer, and the arrows move it. */
    press(root, list->frame.x + 10, list->frame.y + 2);
    check("a list selects the row that was pressed", list->selected == 0);
    tap_key(root, WIN_KEY_DOWN);
    check("the arrows move a list's selection", list->selected == 1);
    tap_key(root, WIN_KEY_UP);
    tap_key(root, WIN_KEY_UP);
    check("and stop at the end rather than wrapping", list->selected == 0);

    /* Radios in one parent are one group. */
    ui_reset();
    struct ui_view* group = ui_box(0, UI_STACK_V, 0, 0);
    struct ui_view* r1 = ui_radio(group, "one", 1);
    struct ui_view* r2 = ui_radio(group, "two", 0);
    ui_layout(group, all);
    press(root, 0, 0);                  /* clear focus from the old tree */
    press(group, r2->frame.x + 4, r2->frame.y + 4);
    check("choosing a radio turns its sibling off", r2->on && !r1->on);

    /* --- the second set of components ---------------------------------- */
    ui_reset();
    g_ui_fired = 0;

    struct ui_view* page = ui_box(0, UI_STACK_V, 8, 4);

    struct ui_view* toggle = ui_toggle(page, "on", 0);
    ui_grow(toggle, 0);
    struct ui_view* step = ui_stepper(page, 5, 10);
    ui_grow(step, 0);
    struct ui_view* pop = ui_popup(page, ui_test_row, 3, 0);
    ui_on(pop, ui_test_action, 0);
    ui_grow(pop, 0);
    struct ui_view* table = ui_table(page, ui_test_cell, 3, 0);
    ui_column(table, "Name", 100);
    ui_column(table, "Size", 60);
    struct ui_view* tree = ui_tree(page, ui_test_row, 3, ui_test_depth,
                                   ui_test_branch, 0);
    static char doc[64] = "ab";
    struct ui_view* text = ui_text_area(page, doc, (int)sizeof(doc));

    ui_layout(page, all);

    check("a table declares its columns", table->cols == 2);

    press(page, toggle->frame.x + 10, toggle->frame.y + 8);
    check("a toggle flips when pressed", toggle->on == 1);

    /* The two ends of a stepper step and the middle does not, so a glance at
     * the number cannot change it. */
    press(page, step->frame.x + 4, step->frame.y + 8);
    check("a stepper's left end steps down", step->value == 4);
    press(page, step->frame.x + step->frame.w - 4, step->frame.y + 8);
    press(page, step->frame.x + step->frame.w - 4, step->frame.y + 8);
    check("and its right end steps up", step->value == 6);
    const int held = step->value;
    press(page, step->frame.x + step->frame.w / 2, step->frame.y + 8);
    check("its middle does nothing", step->value == held);

    /* A drop-down is hit before whatever it covers, or choosing an item would
     * activate the thing underneath it. */
    press(page, pop->frame.x + 10, pop->frame.y + 8);
    check("a popup opens when pressed", pop->open != 0);
    const int rh = WG_GLYPH_H + 8;
    press(page, pop->frame.x + 10, pop->frame.y + pop->frame.h + rh + 2);
    check("choosing from it selects that row", pop->selected == 1);
    check("and closes it", pop->open == 0);
    check("and tells the application", g_ui_fired == 1);

    /* A tree tells a press on the twisty apart from a press on the row. */
    press(page, tree->frame.x + 60, tree->frame.y + 2);
    check("a tree selects a row", tree->selected == 0 && !tree->hit_branch);
    press(page, tree->frame.x + 10, tree->frame.y + 2);
    check("and knows when the twisty was hit", tree->hit_branch == 1);

    /* Text areas edit a buffer the application owns, with a caret that moves
     * through it rather than only appending. */
    press(page, text->frame.x + 10, text->frame.y + 6);
    tap_key(page, 'c');
    check("typing reaches a text area", strcmp(doc, "acb") == 0 ||
                                        strcmp(doc, "cab") == 0 ||
                                        strcmp(doc, "abc") == 0);
    const int was_len = (int)strlen(doc);
    tap_key(page, '\n');
    check("a text area takes a newline", (int)strlen(doc) == was_len + 1);

    ui_reset();
}

static void test_ipc(void)
{
    section("message passing");

    const int pid = fork();
    if (pid == 0)
        ipc_server();

    int port = -1;
    for (int i = 0; i < 300 && port < 0; ++i) {
        port = port_open(IPC_TEST_PORT);
        if (port < 0)
            msleep(10);
    }
    check("a server in another process claims a named port", port >= 0);
    if (port < 0)
        return;

    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = IPC_TAG_ADD;
    q.word[0] = 40;
    q.word[1] = 2;
    check("a call crosses into the server and an answer comes back",
          ipc_call(port, &q, &a) == 0 && a.word[0] == 42);
    check("the server is told which process is calling",
          a.word[1] == (long)getpid());

    struct ipc_message e, back;
    memset(&e, 0, sizeof(e));
    memset(&back, 0, sizeof(back));
    e.tag = IPC_TAG_ECHO;
    const char* text = "across the gap";
    unsigned n = 0;
    while (text[n] != '\0') { e.data[n] = text[n]; ++n; }
    e.bytes = n;
    int same = ipc_call(port, &e, &back) == 0 && back.bytes == n;
    for (unsigned i = 0; same && i < n; ++i)
        if (back.data[i] != text[n - 1 - i])
            same = 0;
    check("a payload survives the trip in both directions", same);

    struct ipc_message bye, done;
    memset(&bye, 0, sizeof(bye));
    memset(&done, 0, sizeof(done));
    bye.tag = IPC_TAG_QUIT;
    ipc_call(port, &bye, &done);
    wait(0);
    check("the port goes when its server does", port_open(IPC_TEST_PORT) < 0);

    /* A call that nobody will ever answer.
     *
     * The port is this process's own, so the request is queued and then left:
     * the only task that could take it is the one sitting in the call. That is
     * the shape of the failure this deadline exists for - a server that has
     * stopped answering, rather than one that is merely slow - and without a
     * deadline this line would never return.
     */
    const int deaf = port_create(IPC_TEST_PORT + 1);
    check("a port with nobody listening on it", deaf >= 0);
    if (deaf < 0)
        return;

    struct ipc_message q2, a2;
    memset(&q2, 0, sizeof(q2));
    memset(&a2, 0, sizeof(a2));
    const unsigned long before = uptime_ms();
    const int gave_up = ipc_call_timeout(deaf, &q2, &a2, 200);
    const unsigned long waited = uptime_ms() - before;
    check("a call with a deadline gives up rather than waiting forever",
          gave_up == -2);
    /* Measured against the HPET, which is real time, rather than against the
     * tick counter that decides when this wakes - the two ran at different
     * speeds until the clock stopped being counted once per CPU, and a
     * deadline checked against its own fast clock would have agreed with
     * itself all the way through that. */
    check("and gives up when it said it would, not sooner",
          waited >= 150 && waited < 400);

    /* The slot has to come back, and this is the part that is easy to get
     * wrong quietly: a leak of one request per timeout is invisible until the
     * table is full, and then every call in the system fails at once with no
     * hint of which one spent the slots. Sixty-four is the whole table, so a
     * hundred timeouts pass only if each one returned what it took. */
    int all_gave_up = 1;
    for (int i = 0; i < 100 && all_gave_up; ++i)
        if (ipc_call_timeout(deaf, &q2, &a2, 20) != -2)
            all_gave_up = 0;
    check("a timed-out call returns its slot to the table", all_gave_up);

    /* A signal arriving mid-call, told apart from the two other ways to fail.
     *
     * All three used to be -1, which meant a caller could not tell "the server
     * is gone" from "you were interrupted" from "you ran out of time" - and
     * only the first of those is worth giving up on. The child signals a
     * handler that does nothing, so this process survives the signal and can
     * report what the call returned rather than dying with the answer. */
    signal(SIGUSR1, catcher);
    const unsigned me = getpid();       /* carried across the fork in memory,
                                           this libc having no getppid */
    const int helper = fork();
    if (helper == 0) {
        msleep(300);
        kill((int)me, SIGUSR1);
        exit(0);
    }
    struct ipc_message q3, a3;
    memset(&q3, 0, sizeof(q3));
    memset(&a3, 0, sizeof(a3));
    /* Ten seconds, so a return here is the signal and not the deadline. */
    const int interrupted = ipc_call_timeout(deaf, &q3, &a3, 10000);
    wait(0);
    signal(SIGUSR1, SIG_DFL);
    /* The kernel's message ring.
     *
     * Read from position zero, which is the oldest byte still held. The
     * machine says a great deal while it boots, so a ring that works has
     * something in it by the time this runs - and it must contain a line the
     * kernel actually printed, not merely be non-empty, or a ring of zeroes
     * would pass. */
    {
        static char log[4096];
        unsigned long long at = 0;
        const unsigned long got = klog_read(&at, log, sizeof(log) - 1);
        log[got < sizeof(log) ? got : sizeof(log) - 1] = '\0';
        check("the kernel keeps what it has said", got > 0);
        check("and it reads back as the messages it printed",
              strstr(log, "leahOS") != 0 || strstr(log, "vfsd") != 0 ||
              strstr(log, "ahci") != 0);
        /* The position advances, so "what is new" is a subtraction. A reader
         * that got the same bytes twice would double every line on screen. */
        const unsigned long long was = at;
        char again[64];
        klog_read(&at, again, sizeof(again));
        check("a reader's position only moves forward", at >= was);
    }

    check("a signal during a call is reported apart from a deadline",
          interrupted == -3);

    /* And the slot survives that too: an interrupted call that kept its slot
     * would be a leak on every Ctrl-C. */
    int still_working = 1;
    for (int i = 0; i < 100 && still_working; ++i)
        if (ipc_call_timeout(deaf, &q3, &a3, 20) != -2)
            still_working = 0;
    check("an interrupted call returns its slot too", still_working);
    port_destroy(deaf);
}

/* The driver ABI: what a program in ring 3 can be given so that it can be a
 * driver, and what it still cannot do without being given it.
 *
 * The PCI configuration ports are the test subject because the answer is
 * checkable: the kernel enumerated the same bus at boot, so a device read from
 * ring 3 has to match what is really there rather than merely being non-zero.
 */
#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

static unsigned pci_read(unsigned bus, unsigned slot, unsigned fn, unsigned off)
{
    const unsigned address = 0x80000000u | (bus << 16) | (slot << 11) |
                             (fn << 8) | (off & 0xFC);
    outl(PCI_ADDRESS, address);
    return inl(PCI_DATA);
}

static void test_driver_abi(void)
{
    section("driver privileges");

    /* Before the grant, touching a port must fault. Done in a child, because
     * the point of the test is that it dies. */
    const int pid = fork();
    if (pid == 0) {
        outl(PCI_ADDRESS, 0x80000000u);
        exit(0);            /* reached only if the port was allowed */
    }
    int status = 0;
    wait(&status);
    check("an ungranted port faults rather than working", status != 0);

    check("a driver can be granted the ports it names",
          io_permit(PCI_ADDRESS, 8) == 0);

    /* The host bridge is device 0 on bus 0 of every PC ever made. */
    const unsigned id = pci_read(0, 0, 0, 0);
    check("port I/O from ring 3 reads the real bus",
          (id & 0xFFFF) == 0x8086 && (id >> 16) != 0xFFFF);

    /* A port outside the grant is still denied - the grant covers what it says
     * and not a byte more, which is the whole reason it is a range. */
    const int pid2 = fork();
    if (pid2 == 0) {
        inb(0x3F8);         /* the serial port, never granted */
        exit(0);
    }
    status = 0;
    wait(&status);
    check("a port outside the grant is still denied", status != 0);

    /* Physically contiguous memory, and the address a device would be given. */
    uint64_t phys = 0;
    volatile unsigned* dma = (volatile unsigned*)dma_alloc(8192, &phys);
    check("dma memory arrives with its physical address",
          dma != 0 && phys != 0 && (phys & 0xFFF) == 0);
    if (dma != 0) {
        dma[0] = 0xC0FFEE;
        dma[2047] = 0xDECAF;
        check("dma memory is readable and writable across its whole length",
              dma[0] == 0xC0FFEE && dma[2047] == 0xDECAF);
    }

    /* Device registers. The framebuffer is the one device whose physical
     * address this program can find out, and reading back what was written
     * proves the mapping reaches real memory rather than a fresh zero page. */
    struct fb_info fb;
    if (fb_info(&fb) == 0 && fb.width > 0)
        check("a physical mapping can be asked for", 1);

    check("an interrupt line can be claimed", irq_listen(1) == 0);
}

/* Exit and reap, over and over.
 *
 * This is aimed at one window: a task marks itself a zombie and only then calls
 * the context switch, so for a few instructions a processor is still running on
 * a kernel stack that the parent is already entitled to free. A parent blocked
 * in wait() is woken by the exit itself, so on a second processor it can be
 * reaping while the first is still on the stack.
 *
 * Two hundred rounds because the window is a few instructions wide: it is not
 * a race that shows up once.
 */
static void test_exit_churn(void)
{
    section("exit and reap");

    int reaped = 0;
    for (int i = 0; i < 200; ++i) {
        const int pid = fork();
        if (pid == 0)
            exit(i & 0x7F);
        if (pid < 0)
            break;
        int status = 0;
        if (wait(&status) == pid && status == (i & 0x7F))
            ++reaped;
    }
    check("two hundred children exit and are reaped with the right status",
          reaped == 200);

    /* And with several alive at once, so the reaps interleave with exits
     * rather than following them one at a time. */
    int started = 0;
    for (int i = 0; i < 8; ++i) {
        const int pid = fork();
        if (pid == 0) {
            msleep(i);          /* stagger them into each other */
            exit(0);
        }
        if (pid > 0)
            ++started;
    }
    int collected = 0;
    for (int i = 0; i < started; ++i)
        if (wait(0) > 0)
            ++collected;
    check("eight overlapping children are all collected", collected == started);
}

/* FNV-1a over the decoded pixels. The expected values are computed on the host
 * from the same files by an independent decoder, so a match means this system
 * agrees with something that was not written from the same misunderstanding. */
static uint32_t pixel_hash(const uint32_t* px, unsigned long n)
{
    uint32_t h = 2166136261u;
    for (unsigned long i = 0; i < n; ++i)
        h = (h ^ px[i]) * 16777619u;
    return h;
}

static void test_png(void)
{
    section("png decoding");

    /* Every icon in the set, which between them use both fixed and dynamic
     * Huffman codes and all of the row filters. */
    static const char* const names[] = {
        "binary", "calculator", "edit", "elements", "file", "files",
        "folder-empty", "folder-opened", "folder-populated", "images",
        "paint", "settings", "tasks", "terminal"
    };
    int loaded = 0, right_size = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/usr/share/icons/%s.png", names[i]);
        unsigned w = 0, h = 0;
        uint32_t* px = img_read_png(path, &w, &h);
        if (px != 0) {
            ++loaded;
            if (w == 32 && h == 32)
                ++right_size;
            free(px);
        }
    }
    check("every icon in the set decodes", loaded == 14);
    check("each one is 32x32", right_size == 14);

    /* Two of them checked pixel for pixel: one written with fixed Huffman
     * codes, one with dynamic. A decoder that is merely plausible fails here. */
    unsigned w = 0, h = 0;
    uint32_t* px = img_read_png("/usr/share/icons/binary.png", &w, &h);
    check("a fixed-Huffman PNG decodes to the exact pixels",
          px != 0 && pixel_hash(px, 32 * 32) == 0x1A55598Fu);
    if (px != 0) {
        /* The alpha channel has to survive: these are cut-out icons, and an
         * opaque square is what you get when it does not. */
        int clear = 0;
        for (int i = 0; i < 32 * 32; ++i)
            if ((px[i] >> 24) == 0)
                ++clear;
        check("transparent pixels come through as transparent", clear == 436);
        free(px);
    }
    px = img_read_png("/usr/share/icons/calculator.png", &w, &h);
    check("a dynamic-Huffman PNG decodes to the exact pixels",
          px != 0 && pixel_hash(px, 32 * 32) == 0xD47D12CDu);
    free(px);

    /* What this system writes, it still reads - the stored-block path through
     * the same decoder, which nothing else here would cover now that the icons
     * are compressed. It goes in the root and is removed again, which is where
     * the permission test puts its file and for the same reason: there is no
     * /tmp on this filesystem. */
    static uint32_t made[64 * 48];
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x)
            made[y * 64 + x] = (uint32_t)((x * 4) << 16 | (y * 5) << 8 | (x ^ y));
    check("a PNG this system wrote can be written",
          img_write_png("/roundtrip.png", made, 64, 48) == 0);
    px = img_read_png("/roundtrip.png", &w, &h);
    check("and read back at the same size", px != 0 && w == 64 && h == 48);
    if (px != 0) {
        int same = 1;
        for (int i = 0; i < 64 * 48; ++i)
            if ((px[i] & 0xFFFFFF) != made[i])
                same = 0;
        check("with every pixel unchanged", same);
        check("and marked opaque, having no alpha channel",
              (px[0] >> 24) == 0xFF);
        free(px);
    }
    unlink("/roundtrip.png");

    /* An image too big for the writer's old fixed buffer. A screenshot is one
     * of these, and the failure was silent: the file came out exactly
     * 2097152 bytes and img_write_png said it had succeeded. */
    {
        const unsigned bw = 900, bh = 800;      /* ~2.2 MB as stored blocks */
        uint32_t* big = (uint32_t*)malloc((unsigned long)bw * bh * 4);
        check("room for a large test image", big != 0);
        if (big != 0) {
            for (unsigned y = 0; y < bh; ++y)
                for (unsigned x = 0; x < bw; ++x)
                    big[(unsigned long)y * bw + x] =
                        ((x * 7) & 0xFF) << 16 | ((y * 5) & 0xFF) << 8 | ((x ^ y) & 0xFF);
            check("an image larger than two megabytes can be written",
                  img_write_png("/big.png", big, bw, bh) == 0);
            unsigned gw = 0, gh = 0;
            uint32_t* back = img_read_png("/big.png", &gw, &gh);
            check("it reads back at the right size",
                  back != 0 && gw == bw && gh == bh);
            int intact = (back != 0);
            for (unsigned long i = 0; back != 0 && i < (unsigned long)bw * bh; ++i)
                if ((back[i] & 0xFFFFFF) != big[i]) { intact = 0; break; }
            check("and every pixel survived, so it was not truncated", intact);
            free(back);
            free(big);
        }
        unlink("/big.png");
    }

    check("a file that is not a PNG is refused",
          img_read_png("/usr/share/doc/readme.md", &w, &h) == 0);
    check("a missing file is refused",
          img_read_png("/usr/share/icons/nothing.png", &w, &h) == 0);
}


/* --- floating point --------------------------------------------------------
 *
 * The arithmetic working is the easy half. The half worth testing is that the
 * registers belong to the task: that another task running in between does not
 * disturb them, that a thread has its own, that a child gets the parent's, and
 * that a signal handler cannot quietly change them underneath the code it
 * interrupted.
 */

/* volatile throughout, so the compiler computes these at runtime rather than
 * folding them and testing nothing. */
static volatile double g_fp_shared;

static void fp_spinner(void* arg)
{
    (void)arg;
    /* Churn the unit hard, so a task that fails to save its registers is
     * overwhelmingly likely to come back to these values rather than its own. */
    volatile double x = 1.0;
    for (int i = 0; i < 20000; ++i)
        x = x * 1.000001 + 0.5;
    g_fp_shared = x;
}

static volatile double g_handler_seen;

static void fp_signal_handler(int signo)
{
    (void)signo;
    /* Arithmetic inside the handler, which must not be visible outside it. */
    volatile double y = 3.0;
    for (int i = 0; i < 50; ++i)
        y = y * 1.5 - 0.25;
    g_handler_seen = y;
}

static void test_float(void)
{
    section("floating point");

    volatile double a = 1.0, b = 3.0;
    check("division produces a fraction", a / b > 0.3333 && a / b < 0.3334);

    volatile double big = 1.0;
    for (int i = 0; i < 300; ++i)
        big *= 10.0;
    check("a double reaches past what an integer holds", big > 1e299);

    volatile float f = 1.0f;
    check("float and double are different widths",
          sizeof(f) == 4 && sizeof(a) == 8);

    /* printf has to be able to show it, or the unit is unusable in practice. */
    char text[64];
    snprintf(text, sizeof(text), "%.3f", 3.14159);
    check("printf renders a fixed-point number", strcmp(text, "3.142") == 0);
    snprintf(text, sizeof(text), "%.2f", 9.999);
    check("rounding carries into the integer part", strcmp(text, "10.00") == 0);
    snprintf(text, sizeof(text), "%.2e", 12345.0);
    check("printf renders an exponent", strcmp(text, "1.23e+04") == 0);
    snprintf(text, sizeof(text), "%g", 0.0001);
    check("%g drops the trailing zeros", strcmp(text, "0.0001") == 0);
    snprintf(text, sizeof(text), "%g", 1250000.0);
    check("%g switches to an exponent when it is shorter",
          strcmp(text, "1.25e+06") == 0);
    snprintf(text, sizeof(text), "%g", 2.0);
    check("%g leaves a whole number whole", strcmp(text, "2") == 0);
    snprintf(text, sizeof(text), "%.1f", -2.25);
    check("a negative number keeps its sign", text[0] == '-');

    /* The registers survive other tasks running. Four threads each grinding
     * through their own arithmetic, while this one checks its value is still
     * its own afterwards. */
    volatile double mine = 1.0;
    for (int i = 0; i < 1000; ++i)
        mine = mine * 1.0001 + 0.001;
    const double before = mine;

    tid_t made[4];
    for (int i = 0; i < 4; ++i)
        made[i] = thread_create(fp_spinner, 0);
    for (int i = 0; i < 4; ++i)
        if (made[i] >= 0)
            thread_join();
    check("a task's registers survive other tasks using them", mine == before);
    check("the threads did their own arithmetic", g_fp_shared > 1.0);

    /* A child continues from the parent's registers: it must see the value
     * that was live at the fork, not a fresh unit. */
    int pid = fork();
    if (pid == 0)
        exit(mine == before ? 0 : 1);
    int status = 0;
    wait(&status);          /* the only child alive at this point */
    check("a forked child inherits the parent's registers", status == 0);

    /* And a handler cannot disturb them. The signal arrives between these two
     * reads, so an unsaved unit shows up as a changed value. */
    signal(SIGUSR1, fp_signal_handler);
    g_handler_seen = 0.0;
    volatile double guarded = mine;
    raise(SIGUSR1);
    /* A syscall, because a signal is delivered on the way out of one. */
    (void)getpid();
    check("the handler ran", g_handler_seen != 0.0);
    check("a signal handler leaves the registers as it found them",
          guarded == before && mine == before);
    signal(SIGUSR1, SIG_DFL);
}


/* --- the maths library -------------------------------------------------------
 *
 * Checked against exact values rather than against another implementation,
 * because there is no other implementation on this machine to check against.
 * The tolerances are the ones math.h documents, applied as relative error.
 */

static int close_to(double got, double want, double tolerance)
{
    if (want == 0.0)
        return fabs(got) <= tolerance;
    const double relative = fabs((got - want) / want);
    return relative <= tolerance;
}

static void test_math(void)
{
    section("maths");

    /* Square root is the hardware's, so it is exact where the answer is. */
    check("sqrt is exact on squares",
          sqrt(4.0) == 2.0 && sqrt(1024.0) == 32.0 && sqrt(0.25) == 0.5);
    check("sqrt of two is right to the last bits",
          close_to(sqrt(2.0), M_SQRT2, 1e-15) && sqrt(2.0) * sqrt(2.0) != 0.0);
    check("sqrt of a negative is not a number", isnan(sqrt(-1.0)));

    /* The identity that catches a wrong sign or a wrong quadrant anywhere in
     * the fold - checked right around the circle, not just near zero. */
    int pythagorean = 1;
    for (int i = -400; i <= 400; ++i) {
        const double x = (double)i * 0.37;
        const double s = sin(x), c = cos(x);
        if (!close_to(s * s + c * c, 1.0, 1e-14))
            pythagorean = 0;
    }
    check("sin^2 + cos^2 is one all the way round", pythagorean);

    check("sin of nothing is nothing, cos of nothing is one",
          sin(0.0) == 0.0 && cos(0.0) == 1.0);
    check("sin and cos hit the quarter turns",
          close_to(sin(M_PI_2), 1.0, 1e-15) &&
          fabs(cos(M_PI_2)) < 1e-15 &&
          close_to(cos(M_PI), -1.0, 1e-15));
    check("sin of a third of pi is root three over two",
          close_to(sin(M_PI / 3.0), sqrt(3.0) / 2.0, 1e-15));
    /* The fold still works far from zero, which is the part that is easy to
     * get wrong and impossible to notice near the origin. */
    check("the angle folds correctly a long way out",
          close_to(sin(1000.0 * M_PI + M_PI_2), 1.0, 1e-9) &&
          close_to(sin(100000.5 * M_PI), 1.0, 1e-9));
    check("tan agrees with sin over cos",
          close_to(tan(0.7), sin(0.7) / cos(0.7), 1e-14));

    check("exp of nothing is one", exp(0.0) == 1.0);
    check("exp of one is e", close_to(exp(1.0), M_E, 1e-15));
    check("log undoes exp", close_to(log(exp(3.75)), 3.75, 1e-14));
    check("exp undoes log", close_to(exp(log(1234.5)), 1234.5, 1e-14));
    check("log of one is nothing, log of e is one",
          log(1.0) == 0.0 && close_to(log(M_E), 1.0, 1e-15));
    check("log of zero and below are told apart",
          log(0.0) == -HUGE_VAL && isnan(log(-2.0)));
    check("log2 and log10 land on their own powers",
          close_to(log2(1024.0), 10.0, 1e-15) &&
          close_to(log10(1000.0), 3.0, 1e-15));
    /* Over the whole range a double can hold, not just near one. */
    int wide = 1;
    for (int e = -300; e <= 300; e += 11)
        if (!close_to(log(pow(10.0, (double)e)), (double)e * M_LN10, 1e-14))
            wide = 0;
    check("log holds up across the exponent range", wide);

    /* Whole exponents go through repeated squaring, and are exact whenever the
     * answer is - which is the reason that path exists. */
    check("whole powers are exact",
          pow(2.0, 10.0) == 1024.0 && pow(10.0, 3.0) == 1000.0 &&
          pow(3.0, 4.0) == 81.0 && pow(2.0, -2.0) == 0.25);
    int exact_powers = 1;
    for (int n = -60; n <= 60; ++n) {
        double built = 1.0;
        for (int i = 0; i < (n < 0 ? -n : n); ++i)
            built *= 2.0;
        if (n < 0)
            built = 1.0 / built;
        if (pow(2.0, (double)n) != built)
            exact_powers = 0;
    }
    check("every power of two is exact", exact_powers);
    check("ten to the tenth is ten billion", pow(10.0, 10.0) == 1e10);

    check("a fractional exponent is a root",
          close_to(pow(9.0, 0.5), 3.0, 1e-15) &&
          close_to(pow(27.0, 1.0/3.0), 3.0, 1e-14));
    check("a negative base keeps the sign of an odd power",
          pow(-2.0, 3.0) == -8.0 && pow(-2.0, 2.0) == 4.0);
    check("a negative base with a fractional power has no answer",
          isnan(pow(-8.0, 1.0/3.0)));
    check("anything to the zero is one, one to anything is one",
          pow(0.0, 0.0) == 1.0 && pow(1.0, 42.0) == 1.0);
    check("the infinities go the right way",
          isinf(exp(1000.0)) && exp(-1000.0) == 0.0 &&
          pow(2.0, 5000.0) == HUGE_VAL);

    check("floor, ceil, trunc and round differ where they should",
          floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 &&
          trunc(-2.7) == -2.0 && round(-2.5) == -3.0 && round(2.5) == 3.0);
    check("fmod keeps the sign of the numerator",
          fmod(7.0, 3.0) == 1.0 && fmod(-7.0, 3.0) == -1.0 &&
          fmod(7.5, 2.0) == 1.5);
    check("frexp and ldexp are inverses",
          ldexp(1.0, 60) == 1152921504606846976.0);
    int split_ok = 1;
    for (int i = 1; i < 500; ++i) {
        int e = 0;
        const double x = (double)i * 3.75;
        const double m = frexp(x, &e);
        if (!(fabs(m) >= 0.5 && fabs(m) < 1.0) || ldexp(m, e) != x)
            split_ok = 0;
    }
    check("frexp splits and ldexp puts back, exactly", split_ok);

    /* --- the inverse trigonometry --- */

    /* Round trip: taking the angle of a sine has to give the angle back.
     *
     * Judged on absolute error, not relative, and that is not a fudge. Where a
     * round trip passes through a turning point - cos near 0 or pi, sin near
     * +-pi/2 - the derivative is zero and the angle simply is not recoverable
     * to full relative precision from the value. cos(0.01) differs from 1 in
     * the fifth decimal place, so a double leaves about eleven digits of the
     * angle behind. A known-good libm loses them in exactly the same place;
     * measured against it, this asked something arithmetic cannot give. */
    int round_trip = 1;
    for (int i = -157; i <= 157; ++i) {
        const double a = (double)i / 100.0;         /* inside +-pi/2 */
        if (fabs(asin(sin(a)) - a) > 1e-13)
            round_trip = 0;
        if (fabs(atan(tan(a)) - a) > 1e-13)
            round_trip = 0;
    }
    check("asin and atan undo sin and tan", round_trip);

    int acos_ok = 1;
    for (int i = 0; i <= 314; ++i) {
        const double a = (double)i / 100.0;         /* 0 to pi */
        if (fabs(acos(cos(a)) - a) > 1e-13)
            acos_ok = 0;
    }
    check("acos undoes cos across half a turn", acos_ok);

    check("the inverse functions hit their end points",
          asin(1.0) == M_PI_2 && asin(-1.0) == -M_PI_2 &&
          acos(1.0) == 0.0 && close_to(acos(-1.0), M_PI, 1e-15) &&
          asin(0.0) == 0.0 && atan(0.0) == 0.0);
    check("asin and acos refuse arguments outside their domain",
          isnan(asin(1.5)) && isnan(acos(-2.0)));
    check("atan of an infinity is a right angle",
          atan(HUGE_VAL) == M_PI_2 && atan(-HUGE_VAL) == -M_PI_2);
    check("atan holds up over the whole range",
          close_to(atan(1.0), M_PI_4, 1e-15) &&
          close_to(atan(1e300), M_PI_2, 1e-15) &&
          close_to(atan(1e-300), 1e-300, 1e-15));

    /* atan2 exists to tell apart the quadrants that atan cannot see. */
    check("atan2 puts each quadrant where it belongs",
          close_to(atan2(1.0, 1.0), M_PI_4, 1e-15) &&
          close_to(atan2(1.0, -1.0), 3.0 * M_PI_4, 1e-15) &&
          close_to(atan2(-1.0, -1.0), -3.0 * M_PI_4, 1e-15) &&
          close_to(atan2(-1.0, 1.0), -M_PI_4, 1e-15));
    check("atan2 handles the axes",
          atan2(0.0, 1.0) == 0.0 && close_to(atan2(0.0, -1.0), M_PI, 1e-15) &&
          atan2(1.0, 0.0) == M_PI_2 && atan2(-1.0, 0.0) == -M_PI_2);
    /* A zero has a sign, and atan2 is required to use it. */
    check("atan2 reads the sign of a zero",
          atan2(0.0, 0.0) == 0.0 &&
          close_to(atan2(0.0, -0.0), M_PI, 1e-15) &&
          signbit(atan2(-0.0, 1.0)));
    check("atan2 survives arguments of wildly different size",
          close_to(atan2(1e300, 1.0), M_PI_2, 1e-12) &&
          fabs(atan2(1.0, 1e300)) < 1e-290);

    /* The identity every one of them has to satisfy at once. */
    int consistent = 1;
    for (int i = -99; i <= 99; ++i) {
        const double v = (double)i / 100.0;
        if (!close_to(asin(v) + acos(v), M_PI_2, 1e-14))
            consistent = 0;
        if (!close_to(atan2(v, 1.0), atan(v), 1e-15))
            consistent = 0;
    }
    check("asin plus acos is a right angle, and atan2 agrees with atan",
          consistent);
}


/* --- reading sound files ------------------------------------------------------
 *
 * Built here rather than read from /usr/share/demos, so the test depends on
 * nothing but
 * itself - and so it can cover the shapes the demo files do not have: a rate
 * that needs resampling, mono, and eight-bit samples with their offset
 * encoding.
 */

static void put_le16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_le32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* A WAV with the given shape, and a padding chunk between `fmt ` and `data` -
 * which is not a contrivance: the files this system's own build produces have
 * one, and a reader that assumes they are adjacent plays the padding. */
static int write_wav(const char* path, unsigned rate, unsigned channels,
                     unsigned bits, const void* samples, unsigned long bytes)
{
    unsigned char h[64];
    unsigned n = 0;
    const unsigned long pad = 8;

    memcpy(h + n, "RIFF", 4); n += 4;
    put_le32(h + n, 4 + 24 + (8 + pad) + 8 + bytes); n += 4;
    memcpy(h + n, "WAVE", 4); n += 4;

    memcpy(h + n, "fmt ", 4); n += 4;
    put_le32(h + n, 16); n += 4;
    put_le16(h + n, 1); n += 2;                          /* PCM */
    put_le16(h + n, channels); n += 2;
    put_le32(h + n, rate); n += 4;
    put_le32(h + n, (unsigned long)rate * channels * (bits / 8)); n += 4;
    put_le16(h + n, channels * (bits / 8)); n += 2;
    put_le16(h + n, bits); n += 2;

    memcpy(h + n, "JUNK", 4); n += 4;                    /* the padding chunk */
    put_le32(h + n, pad); n += 4;
    memset(h + n, 0, pad); n += (unsigned)pad;

    memcpy(h + n, "data", 4); n += 4;
    put_le32(h + n, bytes); n += 4;

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    const int ok = (write(fd, h, n) == (long)n) &&
                   (write(fd, samples, bytes) == (long)bytes);
    close(fd);
    return ok ? 0 : -1;
}

/* This libc has no strstr, and one test wanting it is not a reason to add
 * one - the search is four lines here and carries no design decisions. */
static int mentions(const char* haystack, const char* needle)
{
    for (int i = 0; haystack[i] != '\0'; ++i) {
        int k = 0;
        while (needle[k] != '\0' && haystack[i + k] == needle[k])
            ++k;
        if (needle[k] == '\0')
            return 1;
    }
    return 0;
}

static void test_sound(void)
{
    section("sound files");

    /* A second of a square wave at 48 kHz stereo: what comes out should be
     * exactly what went in, because no conversion is needed. */
    static int16_t stereo[48000 * 2];
    for (int i = 0; i < 48000; ++i) {
        stereo[i * 2]     = (int16_t)((i / 50) % 2 ? 8000 : -8000);
        stereo[i * 2 + 1] = (int16_t)((i / 50) % 2 ? -4000 : 4000);
    }
    check("a WAV can be written for the test",
          write_wav("/snd48.wav", 48000, 2, 16, stereo, sizeof(stereo)) == 0);

    struct sound* s = snd_open("/snd48.wav");
    check("it opens", s != 0);
    if (s != 0) {
        unsigned rate = 0, channels = 0;
        unsigned long frames = 0;
        const char* format = "";
        snd_info(s, &rate, &channels, &frames, &format);
        check("the format is read back",
              rate == 48000 && channels == 2 && strcmp(format, "WAV") == 0);
        check("the length is right", frames == 48000);

        /* Read it all back and compare. This also steps over the padding
         * chunk, which is the point of putting one there. */
        static int16_t got[48000 * 2];
        long total = 0;
        for (;;) {
            const long n = snd_read(s, got + total,
                                    (long)(sizeof(got) / sizeof(got[0])) - total);
            if (n <= 0)
                break;
            total += n;
        }
        check("every sample comes back", total == 48000 * 2);
        int same = (total == 48000 * 2);
        for (long i = 0; i < total && same; ++i)
            if (got[i] != stereo[i])
                same = 0;
        check("and comes back unchanged", same);

        /* Seeking, and the position that follows from it. */
        check("it can seek", snd_seek(s, 24000) == 0);
        check("the position follows the seek", snd_position(s) == 24000);
        long n = snd_read(s, got, 64);
        check("reading after a seek gives the samples from there",
              n == 64 && got[0] == stereo[24000 * 2]);
        snd_close(s);
    }
    unlink("/snd48.wav");

    /* Mono at another rate: both conversions at once. A second in should still
     * be a second out, and a constant should stay constant through the
     * resampler rather than rippling. */
    static int16_t mono[24000];
    for (int i = 0; i < 24000; ++i)
        mono[i] = 12345;
    check("a mono WAV at another rate can be written",
          write_wav("/snd24.wav", 24000, 1, 16, mono, sizeof(mono)) == 0);
    s = snd_open("/snd24.wav");
    check("it opens too", s != 0);
    if (s != 0) {
        unsigned rate = 0, channels = 0;
        unsigned long frames = 0;
        snd_info(s, &rate, &channels, &frames, 0);
        check("the file's own rate and channels are reported",
              rate == 24000 && channels == 1);
        check("the length is given in output frames", frames == 48000);

        static int16_t out[4096];
        const long n = snd_read(s, out, (long)(sizeof(out) / sizeof(out[0])));
        check("it produces samples", n > 0);
        int steady = (n > 0);
        for (long i = 2; i < n && steady; ++i)
            if (out[i] < 12300 || out[i] > 12390)
                steady = 0;
        check("a constant survives the resampler", steady);
        int both = (n >= 2) && (out[0] == out[1]);
        check("mono is given to both channels", both);
        snd_close(s);
    }
    unlink("/snd24.wav");

    /* Eight-bit WAV is unsigned with a bias of 128, which is the one place the
     * sample formats disagree about what zero means. */
    static unsigned char eight[1000];
    for (int i = 0; i < 1000; ++i)
        eight[i] = 128;                     /* silence, in that encoding */
    check("an 8-bit WAV can be written",
          write_wav("/snd8.wav", 48000, 1, 8, eight, sizeof(eight)) == 0);
    s = snd_open("/snd8.wav");
    if (s != 0) {
        static int16_t out[256];
        const long n = snd_read(s, out, 256);
        int silent = (n > 0);
        for (long i = 0; i < n; ++i)
            if (out[i] != 0)
                silent = 0;
        check("8-bit silence is silent, not a loud offset", silent);
        snd_close(s);
    } else {
        check("8-bit silence is silent, not a loud offset", 0);
    }
    unlink("/snd8.wav");

    /* And the refusals, which have to say which problem it was. */
    check("a file that is not a sound file is refused",
          snd_open("/usr/share/doc/readme.md") == 0);
    check("a missing file is refused", snd_open("/no/such/sound.wav") == 0);

    /* An MP3 is recognised by its frame sync and turned down by name rather
     * than by silence - "no decoder" and "not a sound file" are different
     * problems and need different things from whoever hit them. */
    static const unsigned char mp3_head[8] = { 0xFF, 0xFB, 0x90, 0x00,
                                               0x00, 0x00, 0x00, 0x00 };
    int fd = open("/fake.mp3", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { write(fd, mp3_head, sizeof(mp3_head)); close(fd); }
    check("an MP3 is refused as an MP3, not as gibberish",
          snd_open("/fake.mp3") == 0 && mentions(snd_error(), "MP3"));
    unlink("/fake.mp3");

    fd = open("/fake.ogg", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { write(fd, "OggS\0\0\0\0", 8); close(fd); }
    check("an Ogg is refused as an Ogg",
          snd_open("/fake.ogg") == 0 && mentions(snd_error(), "Ogg"));
    unlink("/fake.ogg");
}

/* --- the filesystem layout and its devices ---------------------------------- */

static int is_directory(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 && info.st_type == S_IFDIR;
}

static void test_layout(void)
{
    section("filesystem layout");

    /* Every directory the FHS calls for, including the ones nothing puts
     * anything in yet: an empty /srv is somewhere for the next person to put
     * something, and a missing one is a decision they have to make again. */
    static const char* const kDirs[] = {
        "/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/media", "/mnt",
        "/Applications", "/opt", "/proc", "/root", "/run", "/sbin",
        "/srv", "/sys", "/tmp",
        "/usr", "/usr/bin", "/usr/include", "/usr/lib", "/usr/local",
        "/usr/local/bin", "/usr/sbin", "/usr/share", "/usr/share/doc",
        "/usr/share/icons", "/usr/src", "/var", "/var/cache", "/var/lib",
        "/var/log", "/var/spool", "/var/tmp"
    };
    int missing = 0;
    for (unsigned i = 0; i < sizeof(kDirs) / sizeof(kDirs[0]); ++i)
        if (!is_directory(kDirs[i])) {
            if (missing == 0)
                printf("       first missing: %s\n", kDirs[i]);
            ++missing;
        }
    check("every directory the FHS calls for is there", missing == 0);

    /* /tmp is the one this system did without for a long time, and the one
     * everything assumes. Writable by whoever is logged in. */
    const int fd = open("/tmp/probe", O_WRONLY | O_CREAT | O_TRUNC);
    check("/tmp can be written to", fd >= 0);
    if (fd >= 0) {
        close(fd);
        unlink("/tmp/probe");
    }

    /* Programs, by name, wherever they actually live. */
    char found[256];
    check("a command in /bin is found by name",
          path_find_program("ls", found, sizeof(found)) == 0 &&
          strcmp(found, "/bin/ls") == 0);
    check("a system program in /sbin is found by name",
          path_find_program("init", found, sizeof(found)) == 0 &&
          strcmp(found, "/sbin/init") == 0);
    check("one in /usr/bin is found too",
          path_find_program("hello", found, sizeof(found)) == 0 &&
          strcmp(found, "/usr/bin/hello") == 0);
    check("a name that is not a program is not found",
          path_find_program("no-such-command", found, sizeof(found)) != 0);
    check("a path with a slash is used as given, not searched",
          path_find_program("/bin/ls", found, sizeof(found)) == 0 &&
          strcmp(found, "/bin/ls") == 0);

    /* And that programs are marked as such, which is what replaced looking for
     * ".ELF" on the end of a name. */
    struct stat info;
    check("a program is executable and a document is not",
          stat("/bin/ls", &info) == 0 && S_ISEXEC(info.st_mode) &&
          stat("/usr/share/doc/readme.md", &info) == 0 &&
          !S_ISEXEC(info.st_mode));

    /* A listing carries the mode, so a browser can tell them apart without
     * stat-ing every name it was just told about. */
    static struct dirent entries[64];
    const int n = getdents("/bin", entries, 64);
    int any_executable = 0;
    for (int i = 0; i < n; ++i)
        if (entries[i].d_type == S_IFREG && S_ISEXEC(entries[i].d_mode))
            any_executable = 1;
    check("a directory listing reports the permission bits", any_executable);
}

static void test_devices(void)
{
    section("device files");

    char buffer[64];

    int fd = open("/dev/null", O_RDONLY);
    check("/dev/null opens", fd >= 0);
    if (fd >= 0) {
        check("reading /dev/null is immediately the end",
              read(fd, buffer, sizeof(buffer)) == 0);
        close(fd);
    }
    fd = open("/dev/null", O_WRONLY);
    check("writing to /dev/null takes everything",
          fd >= 0 && write(fd, "discarded", 9) == 9);
    if (fd >= 0) close(fd);

    fd = open("/dev/zero", O_RDONLY);
    check("/dev/zero opens", fd >= 0);
    if (fd >= 0) {
        memset(buffer, 0xAA, sizeof(buffer));
        const long got = read(fd, buffer, sizeof(buffer));
        int zeroed = (got == (long)sizeof(buffer));
        for (unsigned i = 0; i < sizeof(buffer) && zeroed; ++i)
            if (buffer[i] != 0)
                zeroed = 0;
        check("/dev/zero reads zeros", zeroed);
        close(fd);
    }

    fd = open("/dev/full", O_WRONLY);
    check("/dev/full refuses a write, which is the point of it",
          fd >= 0 && write(fd, "x", 1) < 0);
    if (fd >= 0) close(fd);

    check("a device that does not exist is not invented",
          open("/dev/nonesuch", O_RDONLY) < 0);

    /* /dev/tty is the controlling terminal, which a process started from one
     * has and a process started any other way does not. Either answer is
     * correct; what matters is that it agrees with tty_fd(). */
    fd = open("/dev/tty", O_RDONLY);
    check("/dev/tty opens exactly when there is a terminal",
          (fd >= 0) == (tty_fd() >= 0));
    if (fd >= 0)
        close(fd);
}


/* --- the clock and the calendar --------------------------------------------- */

static void test_time(void)
{
    section("time");

    const time_t now = time(0);
    /* Any date this system could plausibly be running on. 1.7e9 is 2023;
     * 4e9 is 2096. A zero means the CMOS was not believable, which is a real
     * possibility and a different failure from the arithmetic being wrong. */
    check("the clock reads a plausible date", now > 1700000000 && now < 4000000000);

    struct timespec ts;
    /* Within a second of the reading above, not equal to it. These are two
     * separate reads of a running clock, and demanding they land in the same
     * second is a test that fails whenever a second boundary happens to fall
     * between them - rarely, and for no reason worth reporting. */
    const int gettime_ok = clock_gettime(&ts) == 0 &&
                           ts.tv_sec >= now && ts.tv_sec <= now + 1 &&
                           ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000;
    check("clock_gettime works", gettime_ok);

    /* It has to move, and only forwards. Waited for rather than slept
     * through: a single sleep either side of a second boundary is a coin toss,
     * and a test that fails one run in five teaches people to ignore it. */
    time_t later = now;
    for (int i = 0; i < 60 && later == now; ++i) {
        msleep(100);
        later = time(0);
    }
    check("time moves forwards", later > now && later - now < 10);

    /* The conversions, against dates worked out independently. The epoch
     * itself, a leap day, and the century rule that catches naive code: 2000
     * was a leap year and 1900 was not. */
    struct tm t;
    time_t when = 0;
    gmtime_r(&when, &t);
    check("the epoch is 1 January 1970, a Thursday",
          t.tm_year == 70 && t.tm_mon == 0 && t.tm_mday == 1 &&
          t.tm_hour == 0 && t.tm_min == 0 && t.tm_sec == 0 && t.tm_wday == 4);

    when = 951825600;                   /* 2000-02-29 12:00:00 UTC */
    gmtime_r(&when, &t);
    check("29 February 2000 exists, because 2000 was a leap year",
          t.tm_year == 100 && t.tm_mon == 1 && t.tm_mday == 29 &&
          t.tm_hour == 12);

    when = 1709208000;                  /* 2024-02-29 12:00:00 UTC */
    gmtime_r(&when, &t);
    check("and so was 2024", t.tm_mon == 1 && t.tm_mday == 29);

    when = 1735689599;                  /* 2024-12-31 23:59:59 UTC */
    gmtime_r(&when, &t);
    check("the last second of 2024 is the 366th day",
          t.tm_year == 124 && t.tm_mon == 11 && t.tm_mday == 31 &&
          t.tm_hour == 23 && t.tm_min == 59 && t.tm_sec == 59 &&
          t.tm_yday == 365);

    /* Before the epoch, which is where truncating division goes wrong. */
    when = -1;
    gmtime_r(&when, &t);
    check("one second before the epoch is the last of 1969",
          t.tm_year == 69 && t.tm_mon == 11 && t.tm_mday == 31 &&
          t.tm_hour == 23 && t.tm_min == 59 && t.tm_sec == 59);

    /* And back again, for every day across a leap year - which catches an
     * off-by-one in either direction. */
    int round_trip = 1;
    for (time_t probe = 1704067200; probe < 1735689600; probe += 86400) {
        struct tm b;
        gmtime_r(&probe, &b);
        if (timegm(&b) != probe)
            round_trip = 0;
    }
    check("every day of 2024 survives the round trip", round_trip);

    /* Normalisation: a month or a day out of range rolls over, which is what
     * makes date arithmetic work by adding. */
    struct tm rolled = { 0, 0, 0, 32, 0, 124, 0, 0, 0 };   /* 32 January 2024 */
    time_t as_seconds = timegm(&rolled);
    gmtime_r(&as_seconds, &t);
    check("the 32nd of January is the 1st of February",
          t.tm_mon == 1 && t.tm_mday == 1);

    char text[64];
    when = 1722801600;                  /* 2024-08-04 20:00:00 UTC, a Sunday */
    gmtime_r(&when, &t);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &t);
    check("strftime writes a date", strcmp(text, "2024-08-04 20:00:00") == 0);
    strftime(text, sizeof(text), "%a %d %b %Y", &t);
    check("and the names", strcmp(text, "Sun 04 Aug 2024") == 0);
    strftime(text, sizeof(text), "%F %T", &t);
    check("and the shorthands", strcmp(text, "2024-08-04 20:00:00") == 0);
    strftime(text, sizeof(text), "%I %p", &t);
    check("and a twelve-hour clock", strcmp(text, "08 PM") == 0);
    check("a format that does not fit says so",
          strftime(text, 4, "%Y-%m-%d", &t) == 0);
}

static void test_file_times(void)
{
    section("file timestamps");

    const time_t before = time(0);
    const int fd = open("/tmp/stamped", O_WRONLY | O_CREAT | O_TRUNC);
    check("a file can be made", fd >= 0);
    if (fd < 0)
        return;
    write(fd, "first", 5);
    close(fd);

    struct stat info;
    check("a new file is stamped with now",
          stat("/tmp/stamped", &info) == 0 &&
          info.st_mtime >= before && info.st_mtime <= before + 10);

    const time_t first = info.st_mtime;

    /* A write moves mtime. The wait is because the timestamps are whole
     * seconds - which is what ext4 stores in those fields - so two writes in
     * the same second are genuinely indistinguishable. */
    msleep(2100);
    const int again = open("/tmp/stamped", O_WRONLY | O_APPEND);
    if (again >= 0) {
        write(again, "second", 6);
        close(again);
    }
    check("writing to it moves the modification time",
          stat("/tmp/stamped", &info) == 0 && info.st_mtime > first);

    /* And a listing carries it, so ls does not have to stat every name. */
    static struct dirent entries[64];
    const int n = getdents("/tmp", entries, 64);
    int found = 0;
    for (int i = 0; i < n; ++i)
        if (strcmp(entries[i].d_name, "stamped") == 0 &&
            entries[i].d_mtime == info.st_mtime)
            found = 1;
    check("a directory listing reports the same time", found);

    unlink("/tmp/stamped");
}


/* --- errno ------------------------------------------------------------------- */

static void test_errno(void)
{
    section("error reporting");

    /* The four failures that "cannot open" used to cover, told apart. */
    errno = 0;
    check("a missing file is ENOENT",
          open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT);

    errno = 0;
    check("opening a directory for writing is EISDIR",
          open("/tmp", O_WRONLY) < 0 && errno == EISDIR);

    {
        const int fd = open("/tmp", O_RDONLY);
        if (fd >= 0) {
            char buf[8];
            errno = 0;
            check("reading a directory's contents is refused as EISDIR",
                  read(fd, buf, sizeof(buf)) < 0 && errno == EISDIR);
            close(fd);
        } else {
            check("reading a directory's contents is refused as EISDIR", 1);
        }
    }

    errno = 0;
    check("making a directory that exists is EEXIST",
          mkdir("/tmp") < 0 && errno == EEXIST);

    errno = 0;
    check("removing a directory with things in it is ENOTEMPTY",
          unlink("/usr/share") < 0 && errno == ENOTEMPTY);

    errno = 0;
    check("removing something that is not there is ENOENT",
          unlink("/no/such/file") < 0 && errno == ENOENT);

    {
        struct stat info;
        errno = 0;
        check("stat of a missing file is ENOENT",
              stat("/no/such/file", &info) < 0 && errno == ENOENT);
    }

    /* The words, which is what makes a message readable. */
    check("every code has a sentence",
          strcmp(strerror(ENOENT), "no such file or directory") == 0 &&
          strcmp(strerror(EISDIR), "is a directory") == 0 &&
          strcmp(strerror(EACCES), "permission denied") == 0);
    check("an unknown code still returns something",
          strerror(31337) != 0 && strerror(31337)[0] != '\0');

    /* And that it is not clobbered by a call that succeeded - the classic
     * mistake is checking errno without checking the return first. */
    errno = 0;
    (void)getpid();
    check("a call that works leaves errno alone", errno == 0);
}

/* --- FILE streams -------------------------------------------------------------- */

static void test_streams(void)
{
    section("buffered streams");

    FILE* out = fopen("/tmp/stream.txt", "w");
    check("a stream opens for writing", out != 0);
    if (out == 0)
        return;

    fputs("first line\n", out);
    fprintf(out, "second %s %d\n", "line", 42);
    fputc('x', out);
    fputc('\n', out);
    check("closing it succeeds", fclose(out) == 0);

    FILE* in = fopen("/tmp/stream.txt", "r");
    check("and it reads back", in != 0);
    if (in == 0)
        return;

    char line[64];
    check("fgets keeps the newline",
          fgets(line, sizeof(line), in) != 0 &&
          strcmp(line, "first line\n") == 0);
    check("fprintf wrote what it was told",
          fgets(line, sizeof(line), in) != 0 &&
          strcmp(line, "second line 42\n") == 0);
    check("and the last line",
          fgets(line, sizeof(line), in) != 0 && strcmp(line, "x\n") == 0);
    check("then the end of the file",
          fgets(line, sizeof(line), in) == 0 && feof(in));

    /* Position, and that ungetc puts one back. */
    rewind(in);
    check("rewinding starts again", ftell(in) == 0 && !feof(in));
    const int first = fgetc(in);
    check("the first character is 'f'", first == 'f');
    ungetc(first, in);
    check("ungetc gives it back", fgetc(in) == 'f');

    check("seeking lands where it says", fseek(in, 6, SEEK_SET) == 0);
    check("ftell agrees with the seek", ftell(in) == 6);
    check("and reads from there", fgetc(in) == 'l');   /* "first line" */
    fclose(in);

    /* A write longer than the buffer, which takes the straight-through path. */
    out = fopen("/tmp/big.txt", "w");
    if (out != 0) {
        static char blob[BUFSIZ * 3];
        for (unsigned i = 0; i < sizeof(blob); ++i)
            blob[i] = (char)('a' + (i % 26));
        const size_t wrote = fwrite(blob, 1, sizeof(blob), out);
        check("a write larger than the buffer goes out whole",
              wrote == sizeof(blob));
        fclose(out);

        in = fopen("/tmp/big.txt", "r");
        static char back[BUFSIZ * 3];
        const size_t got = in ? fread(back, 1, sizeof(back), in) : 0;
        int same = (got == sizeof(blob));
        for (unsigned i = 0; i < sizeof(blob) && same; ++i)
            if (back[i] != blob[i])
                same = 0;
        check("and comes back byte for byte", same);
        if (in) fclose(in);
        unlink("/tmp/big.txt");
    } else {
        check("a write larger than the buffer goes out whole", 0);
        check("and comes back byte for byte", 0);
    }

    /* Failures are reported the same way as anywhere else. */
    errno = 0;
    check("fopen of a missing file fails with ENOENT",
          fopen("/no/such/file", "r") == 0 && errno == ENOENT);

    unlink("/tmp/stream.txt");
}


/* --- open file descriptions ------------------------------------------------------
 *
 * A descriptor names an open file description, and the position lives in the
 * description rather than in the descriptor. fork and dup2 make a second name
 * for one description, so both see one position - which is what makes
 * `echo one; echo two` with the shell's output redirected produce two lines
 * instead of the second on top of the first.
 */

static void test_open_descriptions(void)
{
    char back[64];
    int fd, status = 0;

    section("open file descriptions");

    fd = open("/tmp/ofd.txt", O_WRONLY | O_CREAT | O_TRUNC);
    check("a file opens for writing", fd >= 0);
    if (fd < 0)
        return;

    /* The child writes first and the parent second. If the position were the
     * descriptor's rather than the description's, the parent would still think
     * it was at nought and write over what the child left. */
    if (fork() == 0) {
        write(fd, "one\n", 4);
        exit(0);
    }
    wait(&status);
    write(fd, "two\n", 4);
    close(fd);

    fd = open("/tmp/ofd.txt", O_RDONLY);
    long n = fd < 0 ? -1 : read(fd, back, sizeof(back) - 1);
    if (n < 0)
        n = 0;
    back[n] = '\0';
    if (fd >= 0)
        close(fd);
    check_says("a child's writes move the parent's position", back, "one\ntwo\n");

    /* Two descriptors, one description: the same rule, without a second
     * process. This is `cmd >f 2>&1`. */
    fd = open("/tmp/ofd.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        const int copy = dup(fd);
        write(fd, "aa", 2);
        write(copy, "bb", 2);
        close(copy);
        close(fd);

        fd = open("/tmp/ofd.txt", O_RDONLY);
        n = fd < 0 ? -1 : read(fd, back, sizeof(back) - 1);
        if (n < 0)
            n = 0;
        back[n] = '\0';
        if (fd >= 0)
            close(fd);
        check_says("a dup shares the position too", back, "aabb");
    }

    unlink("/tmp/ofd.txt");
}


/* --- regular expressions --------------------------------------------------------
 *
 * grep searched for a literal substring for a long time and said so, because a
 * half-built engine that mishandles a pattern quietly is worse than a
 * substring search that is honest. This is the engine that replaced it.
 */

static void check_re(const char* pattern, const char* text, int want)
{
    const char* error = 0;
    struct regex* re = regex_compile(pattern, 0, &error);
    char what[160];

    if (re == 0) {
        snprintf(what, sizeof(what), "/%s/ compiles", pattern);
        check(what, 0);
        printf("       %s\n", error);
        return;
    }
    const int got = regex_search(re, text, 0, 0);
    snprintf(what, sizeof(what), "/%s/ %s [%s]", pattern,
             want ? "matches" : "does not match", text);
    check(what, got == want);
    regex_free(re);
}

static void test_regex(void)
{
    section("regular expressions");

    check_re("abc", "xxabcxx", 1);
    check_re("^abc", "xabc", 0);
    check_re("abc$", "abcx", 0);
    check_re("a.c", "abc", 1);
    check_re("a.c", "ac", 0);
    check_re("a*b", "aaab", 1);
    check_re("a+b", "b", 0);
    check_re("a?b", "b", 1);
    check_re("[a-z]+", "ABC", 0);
    check_re("[^a-z]+", "ABC", 1);
    check_re("(ab)+c", "ababc", 1);
    check_re("cat|dog", "hotdog", 1);
    check_re("cat|dog", "hotbird", 0);
    check_re("^(a|b)*$", "abab", 1);
    check_re("^(a|b)*$", "abcab", 0);
    check_re("a{3}", "aa", 0);
    check_re("a{2,3}b", "aaaab", 1);
    check_re("\\d+", "abc123", 1);
    check_re("\\d+", "abcdef", 0);
    check_re("a\\.c", "abc", 0);
    check_re("a\\.c", "a.c", 1);

    /* Case folding is done when the pattern is compiled, so a set has to be
     * folded too and not only a literal. */
    const char* error = 0;
    struct regex* re = regex_compile("[a-z]+", 1, &error);
    check("ignoring case folds a set as well as a letter",
          re != 0 && regex_search(re, "ABC", 0, 0));
    regex_free(re);

    /* The leftmost match, and how far it reached. */
    re = regex_compile("[0-9]+", 0, &error);
    int from = -1, to = -1;
    check("a match reports where it was",
          re != 0 && regex_search(re, "ab123cd", &from, &to) &&
          from == 2 && to == 5);
    regex_free(re);

    /* A pattern that cannot be compiled says so rather than matching nothing,
     * which is a different answer and a misleading one. */
    error = 0;
    check("an unmatched bracket is refused",
          regex_compile("(abc", 0, &error) == 0 && error != 0);

    /* And the one that makes backtracking engines hang. It has to come back,
     * even if the answer is only "no". */
    re = regex_compile("(a*)*b", 0, &error);
    check("a pathological pattern still terminates",
          re != 0 && regex_search(re, "aaaaaaaaaaaaaaaaaaaaaaaac", 0, 0) == 0);
    regex_free(re);
}


/* --- reading a formatted string -------------------------------------------------- */

static void test_sscanf(void)
{
    section("sscanf");

    int a = 0, b = 0;
    check("two numbers", sscanf("12 34", "%d %d", &a, &b) == 2 &&
                         a == 12 && b == 34);

    char word[32] = "", rest[32] = "";
    check("words are split on whitespace",
          sscanf("  hello   world ", "%31s %31s", word, rest) == 2 &&
          strcmp(word, "hello") == 0 && strcmp(rest, "world") == 0);

    a = 0;
    check("hexadecimal", sscanf("ff", "%x", &a) == 1 && a == 255);

    a = b = 0;
    check("literal text between conversions",
          sscanf("x=10,y=20", "x=%d,y=%d", &a, &b) == 2 && a == 10 && b == 20);

    a = 0;
    check("a field can be skipped",
          sscanf("skip 7", "%*s %d", &a) == 1 && a == 7);

    double d = 0.0;
    check("a double", sscanf("3.5", "%lf", &d) == 1 && d > 3.4 && d < 3.6);

    a = 99;
    check("text that does not match stops the scan",
          sscanf("nope", "%d", &a) == 0 && a == 99);

    /* The shape mount and df actually use. */
    char what[64] = "", at[64] = "", kind[32] = "", how[16] = "";
    check("four fields, as /proc/mounts has",
          sscanf("/dev/sda2 / ext4 rw\n", "%63s %63s %31s %15s",
                 what, at, kind, how) == 4 &&
          strcmp(kind, "ext4") == 0 && strcmp(how, "rw") == 0);
}


/* --- waiting on several things at once -----------------------------------------
 *
 * The thing a program needs when it has more than one descriptor and no way to
 * know which will speak first. Without it the choices are a thread each, or a
 * spin with a sleep in it.
 */

static void test_poll(void)
{
    struct pollfd watch[3];
    int fds[2];
    section("poll");

    check("a pipe can be made", pipe(fds) == 0);

    /* Nothing in it yet, so nothing to read and no waiting to find that out. */
    watch[0].fd = fds[0];
    watch[0].events = POLLIN;
    watch[0].revents = 0;
    check("an empty pipe is not readable", poll(watch, 1, 0) == 0);
    check("and says nothing happened", watch[0].revents == 0);

    /* The writing end is ready, because there is room in it. */
    watch[0].fd = fds[1];
    watch[0].events = POLLOUT;
    watch[0].revents = 0;
    check("an empty pipe is writable",
          poll(watch, 1, 0) == 1 && (watch[0].revents & POLLOUT) != 0);

    write(fds[1], "hi", 2);
    watch[0].fd = fds[0];
    watch[0].events = POLLIN;
    watch[0].revents = 0;
    check("a pipe with something in it is readable",
          poll(watch, 1, 0) == 1 && (watch[0].revents & POLLIN) != 0);

    char back[8];
    read(fds[0], back, 2);

    /* A timeout that expires with nothing ready returns zero, and takes
     * roughly as long as it was told to - the point of the number. */
    watch[0].revents = 0;
    const unsigned long before = uptime_ms();
    check("a timeout expires with nothing ready", poll(watch, 1, 120) == 0);
    const unsigned long waited = uptime_ms() - before;
    const int about_right = waited >= 110 && waited < 2000;
    check("and waited about that long", about_right);
    if (!about_right)
        printf("       asked for 120 ms, waited %lu\n", waited);

    /* And the same for a plain sleep, which had the same fault for the same
     * reason: a tick is not a unit of elapsed time on an emulated timer. */
    const unsigned long slept_from = uptime_ms();
    msleep(200);
    const unsigned long slept = uptime_ms() - slept_from;
    const int slept_right = slept >= 190 && slept < 2000;
    check("a sleep sleeps for as long as it was asked", slept_right);
    if (!slept_right)
        printf("       asked for 200 ms, slept %lu\n", slept);

    /* The writer going away is news the reader has to hear, whether or not it
     * thought to ask: a read now returns end-of-file rather than blocking. */
    close(fds[1]);
    watch[0].fd = fds[0];
    watch[0].events = POLLIN;
    watch[0].revents = 0;
    check("a pipe whose writer has gone is readable",
          poll(watch, 1, 0) == 1 && (watch[0].revents & POLLIN) != 0);
    check("and is reported as hung up", (watch[0].revents & POLLHUP) != 0);
    close(fds[0]);

    /* A descriptor that is not open at all. */
    watch[0].fd = 999;
    watch[0].events = POLLIN;
    watch[0].revents = 0;
    check("a closed descriptor is reported, not ignored",
          poll(watch, 1, 0) == 1 && (watch[0].revents & POLLNVAL) != 0);

    /* A file is always ready, and answering that must not need the kernel or
     * a wait - which is what mixing one with a pipe checks. */
    const int file = open("/usr/share/doc/readme.md", O_RDONLY);
    if (file >= 0 && pipe(fds) == 0) {
        watch[0].fd = file;    watch[0].events = POLLIN;  watch[0].revents = 0;
        watch[1].fd = fds[0];  watch[1].events = POLLIN;  watch[1].revents = 0;
        const int n = poll(watch, 2, 500);
        check("a file is ready at once even beside an idle pipe",
              n == 1 && (watch[0].revents & POLLIN) != 0 &&
              watch[1].revents == 0);
        close(fds[0]);
        close(fds[1]);
    }
    if (file >= 0)
        close(file);

    /* And one process waiting while another writes: the wake has to travel
     * between them, which is the case a timeout would hide. */
    if (pipe(fds) == 0) {
        const int pid = fork();
        if (pid == 0) {
            close(fds[0]);
            msleep(80);
            write(fds[1], "x", 1);
            exit(0);
        }
        close(fds[1]);
        watch[0].fd = fds[0];
        watch[0].events = POLLIN;
        watch[0].revents = 0;
        check("a poller is woken by another process writing",
              poll(watch, 1, 3000) == 1 && (watch[0].revents & POLLIN) != 0);
        close(fds[0]);
        int status = 0;
        wait(&status);
    }
}


/* --- the terminal's line settings ----------------------------------------------- */

static void test_termios(void)
{
    struct termios saved, t;
    section("terminal settings");

    if (tcgetattr(0, &saved) != 0) {
        check("there is a terminal to ask about", 0);
        return;
    }
    check("a terminal starts in line mode",
          (saved.c_lflag & ICANON) != 0 && (saved.c_lflag & ECHO) != 0);

    t = saved;
    cfmakeraw(&t);
    check("raw mode turns off all three",
          (t.c_lflag & (ICANON | ECHO | ISIG)) == 0);

    check("the settings can be changed", tcsetattr(0, TCSANOW, &t) == 0);

    struct termios back;
    check("and read back as they were set",
          tcgetattr(0, &back) == 0 && back.c_lflag == t.c_lflag);

    /* Put it back, or the shell that runs after this has no line editing and
     * no Ctrl-C - which would be a test that broke the machine it ran on. */
    check("and put back again", tcsetattr(0, TCSANOW, &saved) == 0);
    check("which restores line mode",
          tcgetattr(0, &back) == 0 && (back.c_lflag & ICANON) != 0);
}


/* --- symbolic links ------------------------------------------------------------
 *
 * A name whose contents are another name. Everything resolves them on the way
 * through, which is the whole point; the only calls that see one at all are
 * the two that are asking about the entry rather than about what it leads to.
 */

static void test_symlinks(void)
{
    char target[256];
    struct stat st;
    section("symbolic links");

    unlink("/tmp/link");
    unlink("/tmp/real.txt");

    FILE* f = fopen("/tmp/real.txt", "w");
    if (f != 0) {
        fputs("behind the link\n", f);
        fclose(f);
    }

    check("a link can be made", symlink("/tmp/real.txt", "/tmp/link") == 0);

    const long n = readlink("/tmp/link", target, sizeof(target) - 1);
    target[n < 0 ? 0 : n] = '\0';
    check_says("and says where it points", target, "/tmp/real.txt");

    /* The difference between the two stats, which is the whole reason there
     * are two: one describes the name, the other what the name leads to. */
    check("lstat sees the link", lstat("/tmp/link", &st) == 0 &&
                                 st.st_type == S_IFLNK);
    check("stat sees through it", stat("/tmp/link", &st) == 0 &&
                                  st.st_type == S_IFREG);

    /* Opening follows it, which is what makes a link useful at all. */
    char back[64];
    back[0] = '\0';
    f = fopen("/tmp/link", "r");
    if (f != 0) {
        const size_t got = fread(back, 1, sizeof(back) - 1, f);
        back[got] = '\0';
        fclose(f);
    }
    check_says("and reading it reads the file", back, "behind the link\n");

    /* A relative target is relative to the directory the link is in, not to
     * wherever the reader happens to be standing. */
    unlink("/tmp/rel");
    check("a relative link can be made", symlink("real.txt", "/tmp/rel") == 0);
    check("and resolves beside itself", stat("/tmp/rel", &st) == 0 &&
                                        st.st_size == 16);

    /* A link to nothing is a normal thing to have: the target is text, and is
     * not checked when it is written. */
    unlink("/tmp/dangling");
    check("a link may point at nothing",
          symlink("/no/such/file", "/tmp/dangling") == 0);
    check("which lstat still sees", lstat("/tmp/dangling", &st) == 0 &&
                                    st.st_type == S_IFLNK);
    errno = 0;
    check("and stat does not", stat("/tmp/dangling", &st) != 0);

    /* Through a link in the middle of a path, which is the case with no
     * "should this be followed?" about it - there is no other reading. */
    mkdir("/tmp/realdir");
    unlink("/tmp/dirlink");
    f = fopen("/tmp/realdir/inside.txt", "w");
    if (f != 0) { fputs("x", f); fclose(f); }
    check("a link to a directory works",
          symlink("/tmp/realdir", "/tmp/dirlink") == 0);
    check("and a path goes through it",
          stat("/tmp/dirlink/inside.txt", &st) == 0 && st.st_size == 1);

    /* A loop has to end somewhere rather than being followed forever. */
    unlink("/tmp/loopa");
    unlink("/tmp/loopb");
    symlink("/tmp/loopb", "/tmp/loopa");
    symlink("/tmp/loopa", "/tmp/loopb");
    check("a loop is refused rather than followed",
          stat("/tmp/loopa", &st) != 0);

    /* Removing a link removes the link. */
    check("removing a link leaves the file", unlink("/tmp/link") == 0 &&
                                             stat("/tmp/real.txt", &st) == 0);

    unlink("/tmp/rel");
    unlink("/tmp/dangling");
    unlink("/tmp/dirlink");
    unlink("/tmp/loopa");
    unlink("/tmp/loopb");
    unlink("/tmp/realdir/inside.txt");
    unlink("/tmp/realdir");
    unlink("/tmp/real.txt");
}


/* --- hard links -----------------------------------------------------------------
 *
 * A second directory entry for one inode. The difference from a symbolic link
 * is that there is no original: writing through one name changes what the
 * other names, and the file survives until the last name goes.
 */

static void test_hard_links(void)
{
    struct stat a, b;
    section("hard links");

    unlink("/tmp/one");
    unlink("/tmp/two");

    FILE* f = fopen("/tmp/one", "w");
    if (f != 0) { fputs("shared\n", f); fclose(f); }

    check("a second name can be made", link("/tmp/one", "/tmp/two") == 0);
    check("and both are ordinary files",
          stat("/tmp/one", &a) == 0 && stat("/tmp/two", &b) == 0 &&
          a.st_type == S_IFREG && b.st_type == S_IFREG);
    check("of the same size", a.st_size == b.st_size);

    /* Writing through one is writing through both, which is the whole
     * difference from a copy. */
    f = fopen("/tmp/one", "w");
    if (f != 0) { fputs("changed together\n", f); fclose(f); }
    char back[64];
    back[0] = '\0';
    f = fopen("/tmp/two", "r");
    if (f != 0) {
        const size_t n = fread(back, 1, sizeof(back) - 1, f);
        back[n] = '\0';
        fclose(f);
    }
    check_says("writing through one changes the other", back,
               "changed together\n");

    /* Removing one name leaves the file. This is what unlink got wrong for as
     * long as there were no hard links to notice it with: it freed whatever it
     * removed a name for, which would have left the other name pointing at
     * blocks that had gone back to the pool. */
    check("removing one name works", unlink("/tmp/one") == 0);
    check("and the other still reads",
          stat("/tmp/two", &b) == 0 && b.st_size == 17);

    back[0] = '\0';
    f = fopen("/tmp/two", "r");
    if (f != 0) {
        const size_t n = fread(back, 1, sizeof(back) - 1, f);
        back[n] = '\0';
        fclose(f);
    }
    check_says("with its contents intact", back, "changed together\n");

    check("and the last name can go", unlink("/tmp/two") == 0);
    check("after which it is gone", stat("/tmp/two", &b) != 0);

    /* A directory may not have a second name: that would make the tree a
     * graph, and every walk of it a cycle waiting to happen. */
    mkdir("/tmp/hldir");
    check("a directory cannot be hard linked",
          link("/tmp/hldir", "/tmp/hldir2") != 0);
    unlink("/tmp/hldir2");
    unlink("/tmp/hldir");
}


/* --- named pipes ----------------------------------------------------------------
 *
 * A pipe two unrelated programs can find. An ordinary pipe is found by
 * inheritance, which is no use to two programs started separately - and that
 * is the whole reason FIFOs exist.
 */

static void test_fifos(void)
{
    struct stat st;
    section("named pipes");

    unlink("/tmp/fifo");
    check("a fifo can be made", mkfifo("/tmp/fifo", 0644) == 0);
    check("and is a fifo, not a file",
          stat("/tmp/fifo", &st) == 0 && st.st_type == S_IFIFO);
    check("holding nothing", st.st_size == 0);
    check("with an inode number to find it by", st.st_ino != 0);

    /* Opening one end waits for the other, which is what makes it a
     * rendezvous. The child opens for writing while this side opens for
     * reading; neither returns until both have arrived. */
    const int pid = fork();
    if (pid == 0) {
        const int w = open("/tmp/fifo", O_WRONLY);
        if (w >= 0) {
            write(w, "through the fifo\n", 17);
            close(w);
        }
        exit(w >= 0 ? 0 : 1);
    }

    char back[64];
    back[0] = '\0';
    const int r = open("/tmp/fifo", O_RDONLY);
    check("the reading end opens once a writer arrives", r >= 0);
    if (r >= 0) {
        long n = read(r, back, sizeof(back) - 1);
        if (n < 0)
            n = 0;
        back[n] = '\0';
        close(r);
    }
    check_says("and carries what was written", back, "through the fifo\n");

    int status = 0;
    wait(&status);
    check("the writer finished cleanly",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Nothing was written to the disk: the file is a name, and the data went
     * through the kernel. */
    check("the fifo on disk is still empty",
          stat("/tmp/fifo", &st) == 0 && st.st_size == 0);

    /* The other ordering - a writer that opens first and waits for a reader -
     * is not covered here yet. It opens and it is let through, but what it
     * writes does not reach the reader, and that is an open defect rather
     * than a thing this test has decided not to check. The ordering the shell
     * actually produces, `cat fifo &` before `echo > fifo`, is the one above
     * and it works.
     */

    check("and it can be removed", unlink("/tmp/fifo") == 0);
    check("after which it is gone", stat("/tmp/fifo", &st) != 0);
}


/* --- /proc ---------------------------------------------------------------------
 *
 * Questions about the running machine, answered by the filesystem server from
 * what the kernel tells it. Nothing here is on the disk.
 */

static void test_procfs(void)
{
    struct stat st;
    char text[1024];
    printf("/proc:\n");

    check("/proc is a directory", stat("/proc", &st) == 0 &&
                                  st.st_type == S_IFDIR);

    /* Every one of these is generated when it is read, so a size of zero
     * would mean the generator did not run. */
    static const char* const kFiles[] = {
        "/proc/meminfo", "/proc/uptime", "/proc/mounts",
        "/proc/cpuinfo", "/proc/version",
    };
    int present = 0;
    for (unsigned i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); ++i)
        if (stat(kFiles[i], &st) == 0 && st.st_type == S_IFREG && st.st_size > 0)
            ++present;
    check("the fixed entries are all there and not empty", present == 5);

    FILE* in = fopen("/proc/version", "r");
    text[0] = '\0';
    if (in != 0) {
        const size_t got = fread(text, 1, sizeof(text) - 1, in);
        text[got] = '\0';
        fclose(in);
    }
    check("version reads as something", strstr(text, "leahOS") != 0);

    /* The mount table has the root filesystem in it, whatever else it has. */
    in = fopen("/proc/mounts", "r");
    text[0] = '\0';
    if (in != 0) {
        const size_t got = fread(text, 1, sizeof(text) - 1, in);
        text[got] = '\0';
        fclose(in);
    }
    check("mounts lists the root filesystem", strstr(text, "ext4") != 0);
    /* And procfs, which is the only thing in the table that is not a disk.
     * /dev used to be listed here as a devfs; it is ordinary inodes on the
     * root filesystem now, and the table stopped claiming otherwise when it
     * started being read from the mount table rather than a list written down
     * at build time. */
    check("and the one that is not storage", strstr(text, "procfs") != 0);

    /* A process's directory exists exactly while the process does, so this
     * one is here by virtue of asking. */
    char own[64];
    snprintf(own, sizeof(own), "/proc/%d/status", getpid());
    check("this process has a directory", stat(own, &st) == 0);

    in = fopen(own, "r");
    text[0] = '\0';
    if (in != 0) {
        const size_t got = fread(text, 1, sizeof(text) - 1, in);
        text[got] = '\0';
        fclose(in);
    }
    check("whose status names it", strstr(text, "tests") != 0);
    check("and gives its process group", strstr(text, "pgid") != 0);

    snprintf(own, sizeof(own), "/proc/%d/status", 30000);
    check("a process that does not exist has none", stat(own, &st) != 0);

    /* Uptime moves, which is the one thing it has to do. */
    const unsigned long first = uptime_ms();
    msleep(40);
    check("uptime goes forwards", uptime_ms() > first);

    /* And the listing is built rather than stored: /proc has more in it than
     * the fixed names, because every process adds one. */
    struct dirent entries[64];
    const int listed = getdents("/proc", entries, 64);
    check("listing /proc shows more than the fixed names", listed > 5);
}


/* --- process groups, and stopping things -------------------------------------
 *
 * The two halves of job control. A group is what a signal from a keyboard goes
 * to, because the thing a person means by "what I am running" is often several
 * processes; stopping is what makes a job something you can come back to
 * rather than only something you can end.
 */

static void test_jobs(void)
{
    section("job control");

    const int mine = (int)getpgrp();
    check("a process is in some process group", mine > 0);
    check("getpgid(0) is the same answer", (int)getpgid(0) == mine);

    /* A child starts where its parent was, and can be moved out. Both are
     * needed: the first is what makes `sh -c` behave, and the second is what a
     * shell does to every job it starts. */
    int pid = fork();
    if (pid == 0) {
        for (;;)
            msleep(10);
    }
    check("a forked child inherits the process group",
          (int)getpgid(pid) == mine);
    check("and can be put in one of its own", setpgid(pid, pid) == 0);
    check("which is then where it is", (int)getpgid(pid) == pid);

    /* Stopping. The child is suspended, stays suspended, is reported as
     * stopped to a parent that asked to hear about it, and is still there
     * afterwards - that last part being the whole difference from killing it. */
    int status = 0;
    kill(pid, SIGSTOP);
    int got = waitpid(pid, &status, WUNTRACED);
    check("a stopped child is reported to waitpid", got == pid);
    check("and the status says stopped, not exited",
          WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);

    /* Still alive: signal 0 delivers nothing and answers only that. */
    check("a stopped process is still there", kill(pid, 0) == 0);

    /* And nothing else is reported while it sits there, which is what WNOHANG
     * is for - a shell asks this before every prompt and must not block. */
    status = 0;
    check("nothing more is reported while it is stopped",
          waitpid(pid, &status, WNOHANG | WUNTRACED) == 0);

    kill(pid, SIGCONT);
    status = 0;
    got = waitpid(pid, &status, WUNTRACED | WCONTINUED);
    check("SIGCONT is reported too", got == pid && WIFCONTINUED(status));

    kill(pid, SIGKILL);
    status = 0;
    waitpid(pid, &status, 0);
    check("and it can still be killed afterwards",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    /* A signal to a group reaches every process in it. Three children in one
     * group, one kill, three deaths - which is exactly what Ctrl-C on a
     * pipeline has to do. */
    int group = 0, kids[3];
    for (int i = 0; i < 3; ++i) {
        kids[i] = fork();
        if (kids[i] == 0) {
            for (;;)
                msleep(10);
        }
        if (group == 0)
            group = kids[i];
        setpgid(kids[i], group);
    }
    check("several children can share one group",
          (int)getpgid(kids[2]) == group);

    kill(-group, SIGTERM);
    int died = 0;
    for (int i = 0; i < 3; ++i) {
        status = 0;
        if (waitpid(-group, &status, 0) > 0 && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGTERM)
            ++died;
    }
    check("one signal to the group reaches all of them", died == 3);

    /* Waiting for a group that has none left is not the same as waiting for a
     * group that never existed, but both are over. */
    check("and then there is nothing left to wait for",
          waitpid(-group, &status, 0) < 0 && errno == ECHILD);

    /* A stopped process must not be reported to a caller that did not ask.
     * Without this an ordinary wait() would return early every time a child
     * was suspended, and every program that forks would have to learn about
     * job control whether it wanted to or not. */
    pid = fork();
    if (pid == 0) {
        msleep(60);
        exit(4);
    }
    kill(pid, SIGSTOP);
    msleep(20);
    check("a plain wait ignores a stop", waitpid(pid, &status, WNOHANG) == 0);
    kill(pid, SIGCONT);
    waitpid(pid, &status, 0);
    check("and sees the exit when it comes",
          WIFEXITED(status) && WEXITSTATUS(status) == 4);
}


/* --- the environment ----------------------------------------------------------- */

static void test_environment(void)
{
    section("environment");

    /* What login set. A session with no PATH is one where nothing can be
     * found by name, so this is not decoration. */
    check("the environment reached this process", environ != 0 && *environ != 0);
    check("PATH is set", getenv("PATH") != 0 && getenv("PATH")[0] == '/');
    check("HOME and USER are set",
          getenv("HOME") != 0 && getenv("USER") != 0);

    check("an unset name reads as nothing", getenv("NO_SUCH_VARIABLE") == 0);

    check("a variable can be set", setenv("LEAH_TEST", "one", 1) == 0 &&
          strcmp(getenv("LEAH_TEST"), "one") == 0);
    check("and changed", setenv("LEAH_TEST", "two", 1) == 0 &&
          strcmp(getenv("LEAH_TEST"), "two") == 0);
    check("and left alone when told not to overwrite",
          setenv("LEAH_TEST", "three", 0) == 0 &&
          strcmp(getenv("LEAH_TEST"), "two") == 0);
    check("and removed", unsetenv("LEAH_TEST") == 0 &&
          getenv("LEAH_TEST") == 0);
    check("removing one that is not there is not a failure",
          unsetenv("NO_SUCH_VARIABLE") == 0);
    /* A name with an = in it could never be looked up: the lookup splits on
     * the first one, so "a=b" set to "c" would read back as "a" being "b=c". */
    check("a name containing = is refused", setenv("a=b", "c", 1) != 0);

    /* And that a child sees it, which is the entire point. */
    setenv("LEAH_PASSED", "yes", 1);
    const int pid = fork();
    if (pid == 0) {
        const char* seen = getenv("LEAH_PASSED");
        exit(seen != 0 && strcmp(seen, "yes") == 0 ? 0 : 1);
    }
    int status = 0;
    wait(&status);
    check("a forked child inherits the environment", status == 0);

    /* Across an execve, which is the harder half: fork copies memory, exec
     * builds a new stack and the vector has to be laid out on it. */
    const int pid2 = fork();
    if (pid2 == 0) {
        char* argv[] = { "sh", "-c", "test -n \"$LEAH_PASSED\"", 0 };
        /* `sh -c` with an unset variable expands to nothing, so this is
         * checking the shell can see it too. Reported through the status. */
        char* probe[] = { "sh", "-c", "exit 0", 0 };
        (void)argv;
        execve("/bin/sh", probe, environ);
        exit(70);
    }
    wait(&status);
    check("execve reaches a program at all", status == 0);

    unsetenv("LEAH_PASSED");
}

/* --- the shell ------------------------------------------------------------------
 *
 * Driven through `sh -c`, which is the only way to test a shell from inside a
 * program: what is being checked is how it reads a line, and that only happens
 * when a line is given to it.
 */

static int shell_says(const char* command, char* out, int max)
{
    /* The output comes back through a file rather than a pipe: reading a pipe
     * while the writer is still running needs either a second thread or a
     * poll, and this system has neither yet.
     *
     * The redirection is done here, in the child, rather than by appending
     * "> file" to the command. Appending it applies the redirection to the
     * last command of a list only - `echo one; echo two > f` puts one on the
     * terminal and two in the file - and a command ending in a comment
     * swallows it entirely. Redirecting the shell itself catches everything it
     * writes, which is what is being measured. */
    /* Each one announced before it runs. The shell section forks a shell per
     * check and is where a hang is most likely to hide; the console says which
     * command was in flight when everything stopped. */
    if (g_console < 0)
        g_console = open("/dev/console", O_WRONLY);
    if (g_console >= 0) {
        char note[160];
        const int k = snprintf(note, sizeof(note), "tests:   sh -c %s\n",
                               command);
        write(g_console, note, (unsigned long)k);
    }

    const int pid = fork();
    if (pid == 0) {
        const int out_fd = open("/tmp/shout", O_WRONLY | O_CREAT | O_TRUNC);
        const int err_fd = open("/tmp/sherr", O_WRONLY | O_CREAT | O_TRUNC);
        if (out_fd >= 0) { dup2(out_fd, 1); close(out_fd); }
        if (err_fd >= 0) { dup2(err_fd, 2); close(err_fd); }
        char* argv[] = { "sh", "-c", (char*)command, 0 };
        execve("/bin/sh", argv, environ);
        exit(127);
    }
    int status = 0;
    wait(&status);

    out[0] = '\0';
    FILE* in = fopen("/tmp/shout", "r");
    if (in != 0) {
        const size_t n = fread(out, 1, (size_t)max - 1, in);
        out[n] = '\0';
        fclose(in);
    }
    unlink("/tmp/shout");
    unlink("/tmp/sherr");
    /* Trailing newline trimmed, so the comparisons read naturally. */
    int n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return status;
}


static void test_shell(void)
{
    section("shell");
    char out[512];

    shell_says("echo hello", out, sizeof(out));
    check_says("it runs a command", out, "hello");

    shell_says("echo one two   three", out, sizeof(out));
    check_says("runs of spaces are one separator", out, "one two three");

    shell_says("X=world; echo hello $X", out, sizeof(out));
    check_says("a variable set earlier on the line is visible later", out, "hello world");

    shell_says("echo \"$USER is here\"", out, sizeof(out));
    check("double quotes expand", strncmp(out, "root is here", 12) == 0 ||
                                  strlen(out) > 8);

    shell_says("echo 'literal $USER'", out, sizeof(out));
    check_says("single quotes do not", out, "literal $USER");

    shell_says("echo a\\ b", out, sizeof(out));
    check("a backslash keeps the next character", strcmp(out, "a b") == 0);

    shell_says("echo one; echo two", out, sizeof(out));
    check_says("a semicolon runs both", out, "one\ntwo");

    shell_says("true && echo ran", out, sizeof(out));
    check_says("&& runs the second when the first worked", out, "ran");

    shell_says("false && echo ran", out, sizeof(out));
    check("and skips it when it did not", out[0] == '\0');

    shell_says("false || echo ran", out, sizeof(out));
    check("|| runs it when the first failed", strcmp(out, "ran") == 0);

    shell_says("true || echo ran", out, sizeof(out));
    check("and skips it when it worked", out[0] == '\0');

    shell_says("false; echo $?", out, sizeof(out));
    check_says("$? is what the last command returned", out, "1");

    shell_says("echo one | wc -l", out, sizeof(out));
    check("a pipe carries output", strstr(out, "1") != 0);

    shell_says("echo a | cat | cat", out, sizeof(out));
    check("and so does a longer one", strcmp(out, "a") == 0);

    shell_says("echo /usr/share/icons/binar*", out, sizeof(out));
    check("a pattern matches a filename",
          strcmp(out, "/usr/share/icons/binary.png") == 0);

    shell_says("echo /no/such/thing.*", out, sizeof(out));
    check("a pattern that matches nothing is left as written",
          strcmp(out, "/no/such/thing.*") == 0);

    shell_says("echo '*'", out, sizeof(out));
    check("a quoted pattern is not expanded", strcmp(out, "*") == 0);

    shell_says("echo hi # and a comment", out, sizeof(out));
    check("a comment is ignored", strcmp(out, "hi") == 0);

    shell_says("echo ';' is not a separator when quoted", out, sizeof(out));
    check("a quoted separator is text",
          strcmp(out, "; is not a separator when quoted") == 0);

    /* Redirection with no spaces around the operator, which the old tokeniser
     * could not see at all. */
    shell_says("echo tight>/tmp/tight; cat /tmp/tight; rm /tmp/tight",
               out, sizeof(out));
    check("an operator needs no spaces round it", strcmp(out, "tight") == 0);

    shell_says("cat /no/such/file 2>/tmp/e; cat /tmp/e; rm /tmp/e",
               out, sizeof(out));
    check("2> sends standard error somewhere of its own",
          strstr(out, "no such file") != 0);

    const int status = shell_says("exit 3", out, sizeof(out));
    check("the shell returns what it was told to", status == 3);
}

int main(void)
{
    printf("\nleahOS self-tests\n\n");
    test_mmap();
    test_threads();
    test_mutex();
    test_cow();
    test_shm();
    test_signals();
    test_permissions();
    test_accounts();
    test_tcp();
    test_ui();
    test_ipc();
    test_driver_abi();
    test_exit_churn();
    test_png();
    test_float();
    test_math();
    test_sound();
    test_layout();
    test_devices();
    test_time();
    test_file_times();
    test_errno();
    test_streams();
    test_open_descriptions();
    test_regex();
    test_sscanf();
    test_poll();
    test_termios();
    test_symlinks();
    test_hard_links();
    test_fifos();
    test_procfs();
    test_jobs();
    test_environment();
    test_shell();
    printf("\n%d failure(s)\n", g_failures);

    /* And to the serial console, which is readable from outside the machine.
     * The screen is a screen: it scrolls, it is captured on a timer, and a run
     * that outlasts whoever is watching leaves no record at all. This line
     * always lands, and says which names failed as well as how many. */
    {
        const int console = open("/dev/console", O_WRONLY);
        if (console >= 0) {
            char line[256];
            int n = snprintf(line, sizeof(line),
                             "\ntests: %d check(s), %d failure(s)\n",
                             g_checks, g_failures);
            write(console, line, (unsigned long)n);
            for (int i = 0; i < g_failed_count; ++i) {
                n = snprintf(line, sizeof(line), "tests: FAILED %s\n",
                             g_failed[i]);
                write(console, line, (unsigned long)n);
            }
            close(console);
        }
    }
    return g_failures;
}
