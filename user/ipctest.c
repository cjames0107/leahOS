/* ipctest - prove a message crosses an address space.
 *
 * Forks a server and calls it. The point of the arithmetic is that neither
 * side can reach the other's memory: the parent's request buffer is not mapped
 * in the child and the child's reply buffer is not mapped in the parent, so an
 * answer arriving at all is the whole test.
 */

#include <ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_PORT 900

#define TAG_ADD    1
#define TAG_ECHO   2
#define TAG_QUIT   3

static int server(void)
{
    const int port = port_create(TEST_PORT);
    if (port < 0) {
        printf("  ipc: server cannot claim the port\n");
        return 1;
    }
    for (;;) {
        struct ipc_message m;
        unsigned from = 0;
        const int handle = ipc_recv(port, &m, &from);
        if (handle < 0)
            return 1;
        if (m.tag == TAG_QUIT) {
            struct ipc_message r = {0};
            r.tag = TAG_QUIT;
            ipc_reply(handle, &r);
            port_destroy(port);
            return 0;
        }
        struct ipc_message r = {0};
        r.tag = m.tag;
        if (m.tag == TAG_ADD) {
            r.word[0] = m.word[0] + m.word[1];
            r.word[1] = (long)from;
        } else {
            /* Send the text back reversed, so a reply that merely echoed the
             * caller's own buffer would not pass. */
            unsigned n = m.bytes;
            if (n > IPC_INLINE) n = IPC_INLINE;
            for (unsigned i = 0; i < n; ++i)
                r.data[i] = m.data[n - 1 - i];
            r.bytes = n;
        }
        ipc_reply(handle, &r);
    }
}

int main(void)
{
    const int pid = fork();
    if (pid == 0)
        exit(server());

    /* Wait for the child to claim the port. */
    int port = -1;
    for (int i = 0; i < 200 && port < 0; ++i) {
        port = port_open(TEST_PORT);
        if (port < 0) msleep(10);
    }
    if (port < 0) {
        printf("  ipc: the port never appeared\n");
        return 1;
    }

    int failures = 0;

    struct ipc_message q = {0}, a = {0};
    q.tag = TAG_ADD;
    q.word[0] = 40;
    q.word[1] = 2;
    if (ipc_call(port, &q, &a) != 0 || a.word[0] != 42) {
        printf("  ipc: add returned %ld, wanted 42\n", (long)a.word[0]);
        ++failures;
    }
    if (a.word[1] != (long)getpid()) {
        printf("  ipc: the server saw caller %ld, wanted %d\n",
               (long)a.word[1], getpid());
        ++failures;
    }

    struct ipc_message e = {0}, back = {0};
    e.tag = TAG_ECHO;
    const char* text = "across the gap";
    unsigned n = 0;
    while (text[n] != '\0') { e.data[n] = text[n]; ++n; }
    e.bytes = n;
    if (ipc_call(port, &e, &back) != 0 || back.bytes != n) {
        printf("  ipc: echo came back %u bytes, wanted %u\n", back.bytes, n);
        ++failures;
    } else {
        for (unsigned i = 0; i < n; ++i) {
            if (back.data[i] != text[n - 1 - i]) {
                printf("  ipc: echo differs at %u\n", i);
                ++failures;
                break;
            }
        }
    }

    struct ipc_message bye = {0}, done = {0};
    bye.tag = TAG_QUIT;
    ipc_call(port, &bye, &done);
    wait(0);

    /* And the server is gone, so the port is gone with it. */
    if (port_open(TEST_PORT) >= 0) {
        printf("  ipc: the port outlived its server\n");
        ++failures;
    }

    printf(failures == 0 ? "  ok  ipc: messages cross address spaces\n"
                         : "  ipc: %d failure(s)\n", failures);
    return failures;
}
