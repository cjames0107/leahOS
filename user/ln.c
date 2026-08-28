/* Make a second name for something.
 *
 * Two quite different things wearing one command, as everywhere:
 *
 *   a hard link is another directory entry for the same inode. There is no
 *   original and no copy; the file goes away when the last name does.
 *
 *   a symbolic link is a small file holding a path. It can point at a
 *   directory, at another filesystem, or at nothing at all, and it stops
 *   working if what it names is moved.
 *
 * The default is the hard one, because that is what `ln` has always meant.
 */

#include <errno.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* `ln -s target dir` means `ln -s target dir/<last part of target>`, which is
 * what makes `ln -s /usr/share/doc/readme.md .` do the obvious thing. */
static void resolve_destination(const char* target, const char* given,
                                char* out, unsigned long max)
{
    struct stat st;
    if (stat(given, &st) != 0 || st.st_type != S_IFDIR) {
        snprintf(out, max, "%s", given);
        return;
    }
    const char* base = target;
    for (const char* p = target; *p != '\0'; ++p)
        if (*p == '/' && p[1] != '\0')
            base = p + 1;
    snprintf(out, max, "%s/%s", given, base);
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "[-s] TARGET NAME   (without -s, another name for the same file)",
              "s");
    const int symbolic = cli_flag("-s");
    if (cli_argc() != 2)
        cli_usage();

    const char* target = cli_arg(0);
    char destination[256];
    resolve_destination(target, cli_arg(1), destination, sizeof(destination));

    const int failed = symbolic ? symlink(target, destination)
                                : link(target, destination);
    if (failed != 0) {
        cli_fail("%s: %s", destination, strerror(errno));
        return 1;
    }
    return 0;
}
