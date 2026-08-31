/* httpd - serve a directory over HTTP.
 *
 * The first server this system has been able to run, and the reason sockets
 * were worth adding: everything before it could open a connection and nothing
 * could be the end that was connected to.
 *
 * Deliberately small. One request per connection, HTTP/1.0, GET only, no
 * keep-alive and no chunking - which is not a subset chosen to save effort but
 * the subset that a browser and `fetch` both speak without negotiating
 * anything. What it demonstrates is the connection, and a larger server would
 * demonstrate the same connection with more code in the way.
 *
 * One connection at a time, in this process, and that is a fact about the
 * stack rather than a shortcut. A connection belongs to netd, which hands back
 * a number for it; a descriptor here is that number wearing a descriptor.
 * Closing one closes the connection, and netd cannot see a fork - so a child
 * per connection would have two owners of one connection and whichever closed
 * first would end it under the other. Serving inline is correct; forking would
 * need netd to count references, which is worth doing when something needs the
 * concurrency and is not worth pretending to have now.
 *
 * A request is one read and one write, so a connection is held for about as
 * long as it takes to answer it.
 */

#include <cli.h>
#include <errno.h>
#include <fcntl.h>
#include <socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BODY_MAX 65536

/* What a browser should do with a file, by the end of its name. Everything
 * unrecognised is bytes, which is what octet-stream means and is safer than
 * guessing text and having a browser render a program. */
static const char* kind_of(const char* path)
{
    const char* dot = 0;
    for (const char* p = path; *p != '\0'; ++p)
        if (*p == '.')
            dot = p;
    if (dot == 0)
        return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html";
    if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".md") == 0)
        return "text/plain";
    if (strcmp(dot, ".css") == 0)  return "text/css";
    if (strcmp(dot, ".png") == 0)  return "image/png";
    if (strcmp(dot, ".gif") == 0)  return "image/gif";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    return "application/octet-stream";
}

static void say(int fd, int code, const char* reason, const char* kind,
                const void* body, long length)
{
    char head[256];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.0 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %ld\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           code, reason, kind, length);
    write(fd, head, (unsigned long)n);
    if (body != 0 && length > 0)
        write(fd, body, (unsigned long)length);
}

/* A path that cannot leave the directory being served.
 *
 * Refusing "..", rather than resolving the path and checking where it landed:
 * a request is a string from a stranger, and the shortest rule that is
 * obviously right beats a cleverer one that has to be argued about. */
static int safe_path(const char* request)
{
    if (request[0] != '/')
        return 0;
    for (const char* p = request; *p != '\0'; ++p) {
        if (p[0] == '.' && p[1] == '.')
            return 0;
        if ((unsigned char)*p < 32)
            return 0;
    }
    return 1;
}

/* One request, on an already-accepted connection. */
static void serve(int fd, const char* root)
{
    char request[1024];
    const long got = read(fd, request, sizeof(request) - 1);
    if (got <= 0)
        return;
    request[got] = '\0';

    /* "GET /path HTTP/1.0". Only the first line matters and only its first
     * two words; the headers are read and thrown away, because there is
     * nothing here that any of them would change. */
    if (strncmp(request, "GET ", 4) != 0) {
        say(fd, 501, "Not Implemented", "text/plain", "GET only\n", 9);
        return;
    }
    char path[512];
    int n = 0;
    for (const char* p = &request[4];
         *p != '\0' && *p != ' ' && *p != '\r' && *p != '\n' &&
         n < (int)sizeof(path) - 1; ++p)
        path[n++] = *p;
    path[n] = '\0';

    if (!safe_path(path)) {
        say(fd, 403, "Forbidden", "text/plain", "no\n", 3);
        return;
    }

    char full[768];
    snprintf(full, sizeof(full), "%s%s", root, path);
    /* A directory means its index, which is the one convention a browser
     * relies on. */
    struct stat st;
    if (stat(full, &st) == 0 && st.st_type == S_IFDIR) {
        const int len = (int)strlen(full);
        snprintf(full + len, sizeof(full) - (unsigned long)len, "%sindex.html",
                 len > 0 && full[len - 1] == '/' ? "" : "/");
    }

    static char body[BODY_MAX];
    const long length = cli_read_file(full, body, sizeof(body));
    if (length < 0) {
        say(fd, 404, "Not Found", "text/plain", "not here\n", 9);
        return;
    }
    say(fd, 200, "OK", kind_of(full), body, length);
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-p port] [directory]", "p:");
    const int port = (int)cli_number("-p", 80);
    const char* root = cli_argc() > 0 ? cli_arg(0) : "/usr/share/doc";

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return cli_fail("no socket: %s", strerror(errno)), 1;
    if (bind(listener, 0, (unsigned short)port) != 0)
        return cli_fail("cannot bind port %d", port), 1;
    if (listen(listener, 4) != 0)
        return cli_fail("port %d is busy", port), 1;

    printf("httpd: serving %s on port %d\n", root, port);
    fflush(stdout);

    for (;;) {
        uint32_t peer = 0;
        uint16_t peer_port = 0;
        const int conn = accept(listener, &peer, &peer_port);
        if (conn < 0) {
            /* A failed accept is not a reason to stop serving: the next one
             * may well work, and a server that exits on one is a server that
             * needs watching. */
            msleep(50);
            continue;
        }

        serve(conn, root);
        close(conn);
    }
}
