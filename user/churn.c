/* churn - make and destroy processes as fast as possible.
 *
 * A reproducer, not a test. The 2-CPU panic lands somewhere in the test
 * suite's last section, where two hundred children are forked and reaped, and
 * running the whole suite to reach it takes minutes. This is that section on
 * its own and on a loop, so the window between "boot" and "panic" is seconds.
 *
 * It deliberately does nothing clever: fork, exit, reap, repeat. If the fault
 * needs anything more than process creation and destruction under two CPUs,
 * this will not find it, and that is itself worth knowing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BATCH  16

int main(int argc, char** argv)
{
    int rounds = 40;
    int r, i;

    if (argc > 1) {
        rounds = 0;
        for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; ++i)
            rounds = rounds * 10 + (argv[1][i] - '0');
        if (rounds <= 0)
            rounds = 40;
    }

    printf("churn: %d rounds of %d\n", rounds, BATCH);
    for (r = 0; r < rounds; ++r) {
        for (i = 0; i < BATCH; ++i) {
            const int pid = fork();
            if (pid == 0)
                exit(0);
            if (pid < 0)
                printf("churn: fork failed at round %d\n", r);
        }
        for (i = 0; i < BATCH; ++i)
            wait(0);
        if ((r % 10) == 0)
            printf("churn: round %d\n", r);
    }
    printf("churn: done, %d processes\n", rounds * BATCH);
    return 0;
}
