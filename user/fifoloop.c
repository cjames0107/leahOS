/* fifoloop - run the named-pipe rendezvous until it goes wrong.
 *
 * `tests` has one round of this and fails it about one run in three, which is
 * a rate you can chase for a week. The exchange itself takes a millisecond, so
 * the same fault should be reachable in seconds by doing it hundreds of times
 * instead of once - and a failure that arrives in seconds is a failure you can
 * bisect.
 *
 * What is being tested is the rendezvous, not the pipe. Two processes open
 * opposite ends of a FIFO at the same moment; neither open returns until both
 * have arrived; the writer writes a known string and closes; the reader reads
 * it back. Every part of that is a place for a wakeup to go missing - and
 * since the big kernel lock came off the system call path, nothing makes the
 * check-and-block in the middle of it atomic.
 *
 * Prints only when something is wrong, plus a summary, so a clean run is one
 * line and a broken one says which round and what it got instead.
 */

#include <cli.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH "/tmp/floop"
static const char kMessage[] = "through the fifo\n";
#define MESSAGE_LEN 17

/* How many failures to print before giving up on printing them. The first few
 * are the interesting ones; a thousand identical lines are not. */
#define REPORT_MAX 8

/* The same idea for an ordinary pipe: fork, write down one end, read up the
 * other. No rendezvous - a plain pipe is found by inheritance - so what this
 * exercises is the check-and-block in pipe_read and pipe_write and the
 * reference counting around a close, without the FIFO naming on top.
 *
 * `tests` fails "a pipe carries output" intermittently, which is the same
 * exchange with a shell either side of it. */
static long plain_pipe_round(char* got, long* n)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    const int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        const long put = write(fds[1], kMessage, MESSAGE_LEN);
        close(fds[1]);
        exit(put == MESSAGE_LEN ? 0 : 2);
    }
    close(fds[1]);

    long total = 0;
    for (;;) {
        const long k = read(fds[0], got + total, 63 - total);
        if (k <= 0)
            break;
        total += k;
        if (total >= MESSAGE_LEN)
            break;
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    *n = total;
    return WEXITSTATUS(status);
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-n rounds] [-p]", "n:p#");
    const long rounds = cli_count("-n", 200);
    const int plain = cli_flag("-p");

    if (plain) {
        long failures = 0;
        for (long round = 0; round < rounds; ++round) {
            char got[64];
            long n = 0;
            memset(got, 0, sizeof(got));
            const long exited = plain_pipe_round(got, &n);
            if (n != MESSAGE_LEN || memcmp(got, kMessage, MESSAGE_LEN) != 0) {
                ++failures;
                if (failures <= REPORT_MAX) {
                    printf("pipeloop: round %ld: read %ld of %d, "
                           "writer exited %ld, got \"%s\"\n",
                           round, n, MESSAGE_LEN, exited, got);
                    fflush(stdout);
                }
            }
        }
        printf("pipeloop: %ld rounds, %ld failures\n", rounds, failures);
        fflush(stdout);
        return failures == 0 ? 0 : 1;
    }

    long failures = 0, short_reads = 0, wrong_bytes = 0, no_open = 0;
    long slowest_round = -1;

    for (long round = 0; round < rounds; ++round) {
        /* A fresh one each time. A FIFO with nobody at either end is thrown
         * away, so this exercises the making and the tearing down as well as
         * the exchange - and those are where the reference counting lives. */
        unlink(PATH);
        if (mkfifo(PATH, 0644) != 0) {
            printf("fifoloop: round %ld: mkfifo failed: %s\n", round,
                   strerror(errno));
            return 1;
        }

        const int pid = fork();
        if (pid == 0) {
            const int w = open(PATH, O_WRONLY);
            if (w < 0)
                exit(1);
            const long put = write(w, kMessage, MESSAGE_LEN);
            close(w);
            exit(put == MESSAGE_LEN ? 0 : 2);
        }

        char back[64];
        memset(back, 0, sizeof(back));
        long got = 0;
        const int r = open(PATH, O_RDONLY);
        if (r >= 0) {
            got = read(r, back, sizeof(back) - 1);
            if (got < 0)
                got = 0;
            close(r);
        }

        int status = 0;
        waitpid(pid, &status, 0);

        int bad = 0;
        if (r < 0) {
            ++no_open;
            bad = 1;
        } else if (got != MESSAGE_LEN) {
            ++short_reads;
            bad = 1;
        } else if (memcmp(back, kMessage, MESSAGE_LEN) != 0) {
            ++wrong_bytes;
            bad = 1;
        }

        if (bad) {
            ++failures;
            slowest_round = round;
            if (failures <= REPORT_MAX) {
                /* The bytes as well as the count: a short read and a read of
                 * the wrong thing are different bugs, and the difference is
                 * only visible here. */
                printf("fifoloop: round %ld: open %s, read %ld of %d, "
                       "writer exited %d, got \"%s\"\n",
                       round, r < 0 ? "FAILED" : "ok", got, MESSAGE_LEN,
                       WEXITSTATUS(status), back);
                fflush(stdout);
            }
        }
    }

    unlink(PATH);
    printf("fifoloop: %ld rounds, %ld failures"
           " (%ld short, %ld wrong, %ld unopenable), last at %ld\n",
           rounds, failures, short_reads, wrong_bytes, no_open, slowest_round);
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
