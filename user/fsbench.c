/* fsbench - how long the filesystem takes when several things want it at once.
 *
 * The question this exists to answer is whether the filesystem server should
 * serve more than one request at a time. Boot cannot answer it: init loads one
 * program, waits, loads the next, so there is never a second request
 * outstanding and any amount of concurrency in the server is unreachable. A
 * measurement of boot is a measurement of a workload with nothing to overlap.
 *
 * So this makes the overlap. N processes read different files at the same
 * time, and the number that matters is how the elapsed time changes with N: a
 * server that truly serves one at a time takes N times as long for N readers,
 * and one that overlaps the waiting takes less.
 *
 * Each reader starts at a different point in the list so that they are reading
 * different files rather than queueing behind each other for the same blocks -
 * which would measure the cache and not the server.
 */

#include <fcntl.h>
#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 64
#define CHUNK     4096

static char  g_file[MAX_FILES][192];
static int   g_files;

static void collect(const char* dir)
{
    struct dirent entries[MAX_FILES];
    const int n = getdents(dir, entries, MAX_FILES);
    for (int i = 0; i < n && g_files < MAX_FILES; ++i) {
        if (entries[i].d_type != S_IFREG || entries[i].d_size < CHUNK)
            continue;
        snprintf(g_file[g_files], sizeof(g_file[0]), "%s/%s", dir,
                 entries[i].d_name);
        ++g_files;
    }
}

/* Read this reader's share of the files: every Nth, so that no two readers
 * touch the same file.
 *
 * Sharing the list was the first thing tried and it measured the wrong thing.
 * Readers walking the same files at different offsets spend most of their time
 * hitting blocks another reader has just pulled into the cache, so the total
 * came out far better than linear and none of it was concurrency in the
 * server. Split, the work stays constant as readers are added and the elapsed
 * time answers the actual question: does asking for it in parallel finish any
 * sooner. */
static unsigned long read_all(int which, int of)
{
    static unsigned char buf[CHUNK];
    unsigned long total = 0;
    for (int i = which; i < g_files; i += of) {
        const int fd = open(g_file[i], O_RDONLY);
        if (fd < 0)
            continue;
        long got;
        while ((got = read(fd, buf, sizeof(buf))) > 0)
            total += (unsigned long)got;
        close(fd);
    }
    return total;
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[readers]", "");
    int readers = cli_argc() > 0 ? atoi_simple(cli_arg(0)) : 1;
    if (readers < 1) readers = 1;
    if (readers > 8) readers = 8;

    collect("/usr/bin");
    collect("/bin");
    if (g_files == 0) {
        cli_fail("nothing to read");
        return 1;
    }

    const unsigned long start = uptime_ms();

    for (int i = 1; i < readers; ++i)
        if (fork() == 0) {
            read_all(i, readers);
            exit(0);
        }
    const unsigned long mine = read_all(0, readers);
    for (int i = 1; i < readers; ++i)
        wait(0);

    const unsigned long ms = uptime_ms() - start;
    /* The whole job is the same size whatever the number of readers, so the
     * elapsed time is directly comparable across runs. Bytes shown are this
     * process's share. */
    printf("fsbench: %d reader(s), %d files split, %lu KiB mine, %lu ms\n",
           readers, g_files, mine / 1024, ms);
    return 0;
}
