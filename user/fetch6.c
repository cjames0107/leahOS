/* fetch6 - an HTTP request over IPv6, from the name to the bytes.
 *
 * The whole path in one command: a AAAA record found over DNS, a neighbour
 * found by solicitation, a handshake and a stream over IPv6. Nothing about it
 * is faked from inside this process - the reply has to come back from a real
 * server through two other processes.
 */

#include <net.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print6(const unsigned char* a)
{
    for (int i = 0; i < 16; i += 2) {
        printf("%x", ((unsigned)a[i] << 8) | a[i + 1]);
        if (i < 14) printf(":");
    }
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[host]", "");
    const char* host = cli_argc() > 0 ? cli_arg(0) : "example.com";

    unsigned char address[16];
    if (resolve6(host, address) != 0) {
        cli_fail("no AAAA record for %s", host);
        return 1;
    }
    printf("fetch6: %s is ", host);
    print6(address);
    printf("\n");

    const int conn = tcp_connect6(address, 80);
    if (conn == -2) {
        cli_fail("refused - the segment arrived and was turned down");
        return 1;
    }
    if (conn < 0) {
        cli_fail("no answer - nothing came back at all");
        return 1;
    }
    printf("fetch6: connected\n");

    char request[256];
    snprintf(request, sizeof(request),
             "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (tcp_write(conn, request, strlen(request)) < 0) {
        printf("fetch6: could not send the request\n");
        tcp_close(conn);
        return 1;
    }

    char buffer[256];
    long total = 0, got = 0;
    for (int i = 0; i < 600; ++i) {
        got = tcp_read(conn, buffer, sizeof(buffer) - 1);
        if (got < 0)
            break;
        if (got == 0) {
            if (total > 0) break;       /* the peer finished */
            msleep(10);
            continue;
        }
        if (total == 0) {
            buffer[got] = '\0';
            /* Just the status line: the point is that it is HTTP, not what
             * the page says. */
            for (long k = 0; k < got; ++k)
                if (buffer[k] == '\r') { buffer[k] = '\0'; break; }
            printf("fetch6: %s\n", buffer);
        }
        total += got;
    }
    tcp_close(conn);

    printf("fetch6: %ld bytes over IPv6\n", total);
    if (total > 0) {
        printf("  ok  a page came back over IPv6\n");
        return 0;
    }
    return 1;
}
