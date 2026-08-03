/* v6test - carry real bytes over IPv6, to a real server.
 *
 * Addressing, SLAAC and NDP have been working for a while, and ping6 proves
 * ICMPv6 reaches a neighbour. What had never been exercised is the part above
 * that: a TCP handshake, a stream, and a clean close, all over v6. The host has
 * no IPv6 of its own, so the peer here is QEMU's own gateway - fec0::2 is the
 * host as seen from inside the guest, exactly as 10.0.2.2 is over v4 - with a
 * server listening on the other side of it.
 *
 *   v6test [address] [port]
 *
 * Defaults to fec0::2 port 8099. An address is given in the usual form; "::"
 * is understood, because writing the gateway out in full is a good way to test
 * the parser instead of the stack.
 */

#include <ipc.h>
#include <net.h>
#include <netd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print6(const unsigned char* a)
{
    int i;
    for (i = 0; i < 16; i += 2) {
        printf("%x", ((unsigned)a[i] << 8) | a[i + 1]);
        if (i < 14) printf(":");
    }
}

/* Enough of the text form to write a gateway address down, "::" included. */
static int parse6(const char* text, unsigned char out[16])
{
    unsigned char head[16], tail[16];
    int nhead = 0, ntail = 0;
    int after_gap = 0;
    unsigned value = 0;
    int digits = 0;
    int i;

    memset(out, 0, 16);
    for (i = 0;; ++i) {
        const char c = text[i];
        if (c == ':' || c == '\0') {
            if (digits > 0) {
                unsigned char* into = after_gap ? tail : head;
                int* count = after_gap ? &ntail : &nhead;
                if (*count + 2 > 16)
                    return -1;
                into[(*count)++] = (unsigned char)(value >> 8);
                into[(*count)++] = (unsigned char)(value & 0xFF);
                value = 0; digits = 0;
            } else if (i > 0 && text[i - 1] == ':') {
                if (after_gap && c != '\0')
                    return -1;          /* only one "::" is allowed */
                after_gap = 1;
            }
            if (c == '\0')
                break;
            continue;
        }
        {
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            value = value * 16 + (unsigned)d;
            if (++digits > 4)
                return -1;
        }
    }
    if (nhead + ntail > 16)
        return -1;
    memcpy(out, head, (unsigned long)nhead);
    memcpy(out + 16 - ntail, tail, (unsigned long)ntail);
    return 0;
}

int main(int argc, char** argv)
{
    unsigned char addr[16];
    unsigned port = 8099;
    char request[128];
    char reply[1024];
    int c;
    long n, total = 0;
    int saw_http = 0;

    if (argc > 1) {
        if (parse6(argv[1], addr) != 0) {
            printf("v6test: '%s' is not an address\n", argv[1]);
            return 1;
        }
    } else {
        memset(addr, 0, 16);
        addr[0] = 0xFE; addr[1] = 0xC0; addr[15] = 0x02;   /* fec0::2 */
    }
    if (argc > 2) {
        unsigned i;
        port = 0;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; ++i)
            port = port * 10 + (unsigned)(argv[2][i] - '0');
        if (port == 0) port = 8099;
    }

    printf("v6test: connecting to ");
    print6(addr);
    printf(" port %u\n", port);

    c = tcp_connect6(addr, (unsigned short)port);
    if (c < 0) {
        printf("v6test: FAILED - no connection\n");
        return 1;
    }
    printf("v6test: connected\n");

    snprintf(request, sizeof(request),
             "GET /v6 HTTP/1.0\r\nHost: leahos\r\nConnection: close\r\n\r\n");
    if (tcp_write(c, request, strlen(request)) < 0) {
        printf("v6test: FAILED - could not send\n");
        tcp_close(c);
        return 1;
    }

    for (;;) {
        n = tcp_read(c, reply, sizeof(reply) - 1);
        if (n <= 0)
            break;                      /* 0 is the peer's FIN */
        reply[n] = '\0';
        if (total == 0) {
            printf("v6test: first bytes back: ");
            {
                long k;
                for (k = 0; k < n && k < 40; ++k)
                    printf("%c", reply[k] == '\r' || reply[k] == '\n'
                                     ? ' ' : reply[k]);
            }
            printf("\n");
            if (reply[0] == 'H' && reply[1] == 'T' && reply[2] == 'T' &&
                reply[3] == 'P')
                saw_http = 1;
        }
        total += n;
    }
    tcp_close(c);

    printf("v6test: %ld bytes, stream closed by the peer\n", total);
    if (!saw_http || total == 0) {
        printf("v6test: FAILED - nothing that looks like a reply\n");
        return 1;
    }
    printf("v6test: TCP over IPv6 carried a real exchange\n");

    /* And a datagram, to the same host, echoed back.
     *
     * The receiver is posted before the sender runs, in a child, because
     * NET_UDP_RECV blocks and a datagram that arrives with nobody waiting is
     * dropped on the floor - netd matches an arrival against the waiting list
     * and there is no queue behind it. Sending first and asking afterwards
     * loses the race every time, which is worth writing down because the API
     * gives no hint of it. */
    {
        const int child = fork();
        if (child == 0) {
            const int net = port_open(IPC_PORT_NET);
            struct ipc_message q, a;
            if (net < 0)
                exit(2);
            memset(&q, 0, sizeof(q));
            q.tag = NET_UDP_RECV;
            q.word[0] = 41234;
            memset(&a, 0, sizeof(a));
            if (ipc_call(net, &q, &a) != 0 || a.word[0] < 0 || a.bytes == 0)
                exit(3);
            a.data[a.bytes < sizeof(a.data) ? a.bytes : sizeof(a.data) - 1] = 0;
            printf("v6test: echo says '%s'\n", a.data);
            exit(0);
        }
        if (child < 0) {
            printf("v6test: FAILED - cannot fork a receiver\n");
            return 1;
        }

        msleep(200);                    /* let the receiver get posted */
        {
            const int net = port_open(IPC_PORT_NET);
            struct ipc_message q, a;
            static const char note[] = "leahos udp over ipv6";
            int tries;
            for (tries = 0; tries < 3; ++tries) {
                memset(&q, 0, sizeof(q));
                q.tag = NET6_UDP_SEND;
                memcpy(q.data, addr, 16);
                memcpy(q.data + 16, note, sizeof(note) - 1);
                q.bytes = 16 + (unsigned)(sizeof(note) - 1);
                q.word[1] = (long)(port + 1);
                q.word[2] = 41234;
                memset(&a, 0, sizeof(a));
                /* The first send may only start neighbour discovery; the
                 * answer to that is what makes the route. */
                if (ipc_call(net, &q, &a) == 0 && a.word[0] >= 0)
                    break;
                msleep(300);
            }
            printf("v6test: datagram sent, waiting for the echo\n");
        }

        {
            int status = 0;
            wait(&status);
            if (status != 0) {
                printf("v6test: FAILED - no datagram came back\n");
                return 1;
            }
        }
    }

    printf("v6test: PASS - TCP and UDP both carried over IPv6\n");
    return 0;
}
