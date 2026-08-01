/* ping6 - an ICMPv6 echo, and the discovery that has to happen first.
 *
 * With no argument it pings the router, because on IPv6 there is always one to
 * ask: a machine finds its neighbours and its network by asking rather than by
 * being told, so "who is out there" is a question with an answer before
 * anything has been configured.
 */

#include <ipc.h>
#include <netd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print6(const unsigned char* a)
{
    /* Not the shortest form: the run-of-zeroes rule is a lot of code for a
     * diagnostic, and every group written out is unambiguous. */
    for (int i = 0; i < 16; i += 2) {
        printf("%x", ((unsigned)a[i] << 8) | a[i + 1]);
        if (i < 14) printf(":");
    }
}

/* "fe80::5054:ff:fe12:3456" in, sixteen bytes out. */
static int parse6(const char* text, unsigned char* out)
{
    unsigned groups[8];
    int n = 0, gap = -1, value = 0, digits = 0;
    for (int i = 0; ; ++i) {
        const char c = text[i];
        if (c == ':' || c == '\0') {
            if (digits > 0) {
                if (n >= 8) return -1;
                groups[n++] = (unsigned)value;
            } else if (c == ':' && text[i + 1] == ':') {
                gap = n;
                ++i;
            } else if (c != '\0' && n != 0) {
                return -1;
            }
            value = 0;
            digits = 0;
            if (c == '\0') break;
            continue;
        }
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        value = value * 16 + d;
        if (++digits > 4) return -1;
    }

    memset(out, 0, 16);
    if (gap < 0) {
        if (n != 8) return -1;
        for (int i = 0; i < 8; ++i) {
            out[i * 2] = (unsigned char)(groups[i] >> 8);
            out[i * 2 + 1] = (unsigned char)groups[i];
        }
        return 0;
    }
    /* Everything before the gap sits at the front, everything after at the
     * back, and the zeroes are whatever is left in between. */
    for (int i = 0; i < gap; ++i) {
        out[i * 2] = (unsigned char)(groups[i] >> 8);
        out[i * 2 + 1] = (unsigned char)groups[i];
    }
    const int tail = n - gap;
    for (int i = 0; i < tail; ++i) {
        const int slot = 8 - tail + i;
        out[slot * 2] = (unsigned char)(groups[gap + i] >> 8);
        out[slot * 2 + 1] = (unsigned char)groups[gap + i];
    }
    return 0;
}

int main(int argc, char** argv)
{
    const int port = port_open(IPC_PORT_NET);
    if (port < 0) {
        printf("ping6: no network stack is running\n");
        return 1;
    }

    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NET6_INFO;
    if (ipc_call(port, &q, &a) != 0) {
        printf("ping6: the stack did not answer\n");
        return 1;
    }
    printf("link-local  ");
    print6((unsigned char*)a.data);
    printf("\n");
    if (a.word[0]) {
        printf("global      ");
        print6((unsigned char*)a.data + 16);
        printf("\n");
    } else {
        printf("global      none - no router has advertised a prefix\n");
    }

    unsigned char target[16];
    if (argc > 1) {
        if (parse6(argv[1], target) != 0) {
            printf("ping6: '%s' is not an address\n", argv[1]);
            return 1;
        }
    } else if (a.word[0]) {
        /* The router itself, which on this link is the prefix with a 2 on the
         * end - and is in any case the one address known to be reachable. */
        memcpy(target, a.data + 16, 16);
        memset(target + 8, 0, 8);
        target[15] = 2;
    } else {
        printf("ping6: nothing to ping and no router to ask\n");
        return 1;
    }

    printf("pinging ");
    print6(target);
    printf("\n");

    int replies = 0;
    for (unsigned seq = 1; seq <= 3; ++seq) {
        memset(&q, 0, sizeof(q));
        memset(&a, 0, sizeof(a));
        q.tag = NET6_PING;
        memcpy(q.data, target, 16);
        q.bytes = 16;
        q.word[1] = (long)seq;
        if (ipc_call(port, &q, &a) == 0 && a.word[0] >= 0) {
            printf("  reply seq %u, hop limit %ld\n", seq, (long)a.word[0]);
            ++replies;
        } else {
            printf("  no reply, seq %u\n", seq);
        }
    }
    return replies > 0 ? 0 : 1;
}
