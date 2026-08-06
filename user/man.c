/* The manual.
 *
 * Plain text, not troff. troff is a typesetting language with a macro package
 * on top, and the reason manual pages are written in it is that in 1971 the
 * same source had to drive a phototypesetter - which is not a problem anybody
 * here has. What is left when that goes is a file somebody can read with cat,
 * and a formatter nobody has to write.
 *
 * Paged through `less` when there is a terminal to page onto, because a page
 * longer than the window is otherwise only readable in its last twenty lines.
 * Straight out when there is not, so `man ls | grep -i sort` works.
 */

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAN_DIR "/usr/share/man"

static int page_path(const char* name, char* out, unsigned long max)
{
    struct stat st;
    snprintf(out, max, "%s/%s.1", MAN_DIR, name);
    return stat(out, &st) == 0 && st.st_type == S_IFREG ? 0 : -1;
}

/* Everything there is a page for. `man` with no argument is a reasonable way
 * to ask what the machine can do, and a usage line is not an answer to it. */
static int list_pages(void)
{
    struct dirent entries[128];
    const int n = getdents(MAN_DIR, entries, 128);
    if (n < 0) {
        fprintf(stderr, "man: %s is not there\n", MAN_DIR);
        return 1;
    }

    printf("manual pages in %s:\n", MAN_DIR);
    int on_line = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned long len = strlen(entries[i].d_name);
        if (len < 3 || strcmp(entries[i].d_name + len - 2, ".1") != 0)
            continue;
        printf("  %-12.*s", (int)(len - 2), entries[i].d_name);
        if (++on_line == 5) {
            printf("\n");
            on_line = 0;
        }
    }
    if (on_line != 0)
        printf("\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return list_pages();

    char path[256];
    if (page_path(argv[1], path, sizeof(path)) != 0) {
        fprintf(stderr, "man: no manual entry for %s\n", argv[1]);
        return 1;
    }

    /* Output going to a terminal means a person is reading, and a person wants
     * to be able to stop half way. Anything else - a pipe, a file - wants the
     * whole thing, with no pager waiting for a keypress that is never coming.
     *
     * isatty(1), not tty_fd(): `man ls | head` runs in a process that has a
     * terminal, and paging into a pipe leaves less sitting in the foreground
     * holding it. */
    if (isatty(1)) {
        char less[256];
        if (path_find_program("less", less, sizeof(less)) == 0) {
            char* args[] = { "less", path, 0 };
            execve(less, args, environ);
            /* Only reached if that failed; fall through and just print it. */
        }
    }

    FILE* in = fopen(path, "r");
    if (in == 0) {
        fprintf(stderr, "man: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), in) != 0)
        fputs(line, stdout);
    fclose(in);
    return 0;
}
