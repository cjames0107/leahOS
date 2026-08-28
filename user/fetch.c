/* fetch - an HTTP GET over TCP.
 *
 * Small, but it exercises the entire stack in one go: DNS resolves the name,
 * ARP finds the gateway, TCP opens a connection through it, and the reply comes
 * back through the same file-descriptor machinery as a file or a pipe.
 *
 *   fetch <host> [path]
 */

#include <net.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "HOST [path]", "");
    if (cli_argc() < 1 || cli_argc() > 2)
        cli_usage();
    const char* host = cli_arg(0);
    const char* path = cli_argc() == 2 ? cli_arg(1) : "/";

    uint32_t ip;
    if (parse_ip(host, &ip) < 0 && resolve(host, &ip) < 0) {
        cli_fail("cannot resolve '%s'", host);
        return 1;
    }
    printf("connecting to %s (%u.%u.%u.%u) port 80\n", host,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

    const int conn = tcp_connect(ip, 80);
    if (conn < 0) {
        cli_fail("connection refused or timed out");
        return 1;
    }

    /* HTTP/1.0 so the server closes when it is done and we see a clean end of
     * stream; 1.1 would keep the connection alive waiting for another request. */
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    if (tcp_write(conn, request, strlen(request)) < 0) {
        cli_fail("could not send the request");
        tcp_close(conn);
        return 1;
    }

    long total = 0;
    char buffer[1024];
    for (;;) {
        const long got = tcp_read(conn, buffer, sizeof(buffer) - 1);
        if (got <= 0)
            break;                  /* 0 is end of stream, -1 a dead connection */
        buffer[got] = '\0';
        write(1, buffer, got);
        total += got;
    }

    tcp_close(conn);
    printf("\n--- %ld bytes received ---\n", total);
    return total > 0 ? 0 : 1;
}
