/* fsck - check the filesystem, and put right what can be put right. */

#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>

int main(int argc, char** argv)
{
    int repair = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "-p") == 0)
            repair = 1;
        else {
            printf("usage: fsck [-y]\n");
            printf("  -y  repair what can be repaired, without asking\n");
            return 2;
        }
    }

    char report[4096];
    unsigned fixed = 0;
    const long problems = fsck(repair, report, sizeof(report), &fixed);

    if (report[0] != '\0')
        printf("%s", report);

    if (problems < 0) {
        printf("fsck: the check could not be run\n");
        return 2;
    }
    if (problems == 0)
        return 0;

    printf("fsck: %ld problem%s found", problems, problems == 1 ? "" : "s");
    if (repair)
        printf(", %u repaired", fixed);
    printf("\n");
    /* The convention every fsck follows: nonzero when the disk was changed,
     * so that a boot script knows whether it has to start over. */
    return fixed != 0 ? 1 : 4;
}
