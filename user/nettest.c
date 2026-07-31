/* nettest - ask the stack, which asks the driver, which asks the card.
 *
 * Three processes and two ports between this program and the wire, none of
 * them sharing a byte of memory with the next.
 */

#include <ipc.h>
#include <netd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int port = -1;
    for (int i = 0; i < 400 && port < 0; ++i) {
        port = port_open(IPC_PORT_NET);
        if (port < 0) msleep(10);
    }
    if (port < 0) {
        printf("nettest: no stack is running\n");
        return 1;
    }

    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NET_INFO;
    if (ipc_call(port, &q, &a) != 0) {
        printf("nettest: the stack did not answer\n");
        return 1;
    }
    printf("nettest: %u.%u.%u.%u on %02x:%02x:%02x:%02x:%02x:%02x\n",
           (unsigned)(a.word[0] >> 24) & 0xFF, (unsigned)(a.word[0] >> 16) & 0xFF,
           (unsigned)(a.word[0] >> 8) & 0xFF, (unsigned)a.word[0] & 0xFF,
           a.data[0], a.data[1], a.data[2], a.data[3], a.data[4], a.data[5]);

    const unsigned gateway = (unsigned)a.word[1];

    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NET_RESOLVE;
    q.word[0] = gateway;
    if (ipc_call(port, &q, &a) == 0 && a.word[0] == 0)
        printf("nettest: gateway is at %02x:%02x:%02x:%02x:%02x:%02x\n",
               a.data[0], a.data[1], a.data[2], a.data[3], a.data[4], a.data[5]);
    else
        printf("nettest: could not resolve the gateway\n");

    int replies = 0;
    for (unsigned seq = 1; seq <= 3; ++seq) {
        memset(&q, 0, sizeof(q));
        memset(&a, 0, sizeof(a));
        q.tag = NET_PING;
        q.word[0] = (long)gateway;
        q.word[1] = (long)seq;
        if (ipc_call(port, &q, &a) == 0 && a.word[0] >= 0) {
            printf("nettest: reply from the gateway, seq %u, ttl %ld\n",
                   seq, (long)a.word[0]);
            ++replies;
        } else {
            printf("nettest: no reply, seq %u\n", seq);
        }
    }

    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NET_STATS;
    ipc_call(port, &q, &a);
    printf("nettest: %ld frames in, %ld out, %ld addresses known\n",
           (long)a.word[0], (long)a.word[1], (long)a.word[2]);

    if (replies == 3) {
        printf("  ok  a ping crossed three processes and came back\n");
        return 0;
    }
    return 1;
}
