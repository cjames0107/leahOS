/* leahOS self-tests for the userland-facing kernel features.
 *
 * Run it from the shell as `tests`. Each check prints a pass/fail line, and the
 * exit status is the number of failures, so it doubles as something a script
 * can act on.
 */

#include <fcntl.h>
#include <signal.h>
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
#include <math.h>
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
    printf("threads:\n");

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

static void test_cow(void)
{
    printf("copy-on-write fork:\n");

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
    printf("shared memory:\n");

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
          status == 128 + SIGTERM);

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

static void test_accounts(void)
{
    printf("accounts and authentication:\n");

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
    printf("tcp:\n");

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

static void test_ipc(void)
{
    printf("message passing:\n");

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
    printf("driver privileges:\n");

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
    printf("exit and reap:\n");

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
    printf("png decoding:\n");

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
        snprintf(path, sizeof(path), "/share/icons/%s.png", names[i]);
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
    uint32_t* px = img_read_png("/share/icons/binary.png", &w, &h);
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
    px = img_read_png("/share/icons/calculator.png", &w, &h);
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

    check("a file that is not a PNG is refused",
          img_read_png("/docs/readme.md", &w, &h) == 0);
    check("a missing file is refused",
          img_read_png("/share/icons/nothing.png", &w, &h) == 0);
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
    printf("floating point:\n");

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
    printf("maths:\n");

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
    test_ipc();
    test_driver_abi();
    test_exit_churn();
    test_png();
    test_float();
    test_math();
    printf("\n%d failure(s)\n", g_failures);
    return g_failures;
}
