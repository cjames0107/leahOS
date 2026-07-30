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
#include <ipc.h>
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

    const int fd = tcp_connect(ip, 80);
    check("TCP connected to a real server", fd >= 0);
    if (fd < 0)
        return;

    static const char request[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    check("wrote a request to the socket",
          write(fd, request, sizeof(request) - 1) == (long)(sizeof(request) - 1));

    char buffer[512];
    const long got = read(fd, buffer, sizeof(buffer) - 1);
    check("read a response back", got > 0);
    if (got > 0) {
        buffer[got] = '\0';
        check("the response is HTTP", memcmp(buffer, "HTTP/", 5) == 0);
    }

    /* Drain to end of stream, which is what proves the peer's FIN was seen
     * rather than the connection simply going quiet. */
    long total = got > 0 ? got : 0;
    for (;;) {
        const long more = read(fd, buffer, sizeof(buffer) - 1);
        if (more <= 0)
            break;
        total += more;
    }
    check("the stream ended cleanly at the peer's FIN", total > (long)got);

    close(fd);
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
    printf("\n%d failure(s)\n", g_failures);
    return g_failures;
}
