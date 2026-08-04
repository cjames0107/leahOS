/* Finding a program by name.
 *
 * What a PATH would do, if this system had environment variables to keep one
 * in. The list is compiled in for now, in the order the FHS implies: the
 * ordinary commands, then the system's own, then anything added later.
 *
 * It lives in libc rather than in the shell because the shell is not the only
 * thing that starts programs - login, the terminal, the file browser and the
 * desktop all do - and every one of them used to spell out a path of its own.
 */

#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* const kSearch[] = {
    PATH_BIN, PATH_SBIN, PATH_USR_BIN, PATH_LOCAL_BIN
};

int path_find_program(const char* name, char* out, int max)
{
    struct stat info;

    if (name == 0 || name[0] == '\0' || out == 0 || max <= 0)
        return -1;

    /* Anything with a slash is a path already: ./thing, /sbin/init, or a
     * relative path into a directory. Searching for it would be wrong. */
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == '/') {
            snprintf(out, (unsigned)max, "%s", name);
            return stat(out, &info) == 0 && info.st_type == S_IFREG ? 0 : -1;
        }
    }

    for (unsigned i = 0; i < sizeof(kSearch) / sizeof(kSearch[0]); ++i) {
        snprintf(out, (unsigned)max, "%s/%s", kSearch[i], name);
        if (stat(out, &info) == 0 && info.st_type == S_IFREG)
            return 0;
    }
    out[0] = '\0';
    return -1;
}
