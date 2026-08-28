/* fsck - check the filesystem, and put right what can be put right. */

#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "[-y]   (-y repairs what can be repaired, without asking)",
              "yp");
    const int repair = cli_flag("-y") || cli_flag("-p");

    char report[4096];
    unsigned fixed = 0;
    const long problems = fsck(repair, report, sizeof(report), &fixed);

    if (report[0] != '\0')
        printf("%s", report);

    if (problems < 0) {
        cli_fail("the check could not be run");
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
