/* test, [ - ask a question about a file, a string or a number.
 *
 * One program under two names, as shutdown and reboot are: `[` is `test` that
 * insists on a closing bracket, and they differ in nothing else. The
 * expression itself lives in libc, because the shell evaluates the same one as
 * a builtin - a fork and an exec per comparison inside a loop is most of what
 * the loop would cost - and two copies of the rules would drift.
 *
 * The exit status is the answer: 0 for true, 1 for false, 2 for an expression
 * that does not parse. Nothing is printed, which is the whole point.
 */

#include <stdio.h>
#include <string.h>
#include <testexpr.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    /* No cli_begin: every argument here belongs to the expression, and a
     * leading -f or -n is an operator rather than an option. Even --help would
     * be a string to test rather than a request. */
    const char* name = argc > 0 && argv[0] != 0 ? argv[0] : "test";
    const char* leaf = name;
    for (const char* p = name; *p != '\0'; ++p)
        if (*p == '/')
            leaf = p + 1;

    int n = argc - 1;
    if (strcmp(leaf, "[") == 0) {
        if (n < 1 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing ]\n");
            return 2;
        }
        --n;                            /* the bracket is punctuation */
    }
    return test_expr(n, &argv[1]);
}
