#ifndef _PATHS_H
#define _PATHS_H

/* Where things live.
 *
 * The layout is the Filesystem Hierarchy Standard, which is worth following
 * for the ordinary reason: anyone who has used a UNIX already knows where to
 * look, and every argument about where a new thing goes has been had once
 * already by somebody else.
 *
 * It did not start that way. Programs were at /BIN/NAME.ELF - upper-cased and
 * suffixed because the first filesystem this system could read was FAT, whose
 * names are eight characters and three, upper case. The FAT driver is long
 * gone; the shouting outlived it, along with a hardcoded path in every program
 * that wanted to start another. None of that is here now.
 *
 * These constants exist so that the next move costs one edit rather than
 * twenty. A program that spells "/usr/share/icons" itself is a program that
 * will be wrong later.
 */

/* Programs. Searched in this order by path_find_program, which is what a PATH
 * would do if this system had environment variables to keep one in. */
#define PATH_BIN        "/bin"          /* the ordinary commands */
#define PATH_SBIN       "/sbin"         /* the system's own: init, the servers */
#define PATH_USR_BIN    "/usr/bin"
#define PATH_LOCAL_BIN  "/usr/local/bin"

/* Application bundles. FHS reserves /opt for add-on software packages, each in
 * its own directory, which is what a .app is. */
#define PATH_APPS       "/opt"

/* Read-only data shared between programs. */
#define PATH_SHARE      "/usr/share"
#define PATH_ICONS      "/usr/share/icons"
#define PATH_FONTS      "/usr/share/fonts"
#define PATH_GLYPHS     "/usr/share/icons/glyphs"
#define PATH_WALLPAPERS "/usr/share/wallpapers"
#define PATH_DOC        "/usr/share/doc"
#define PATH_DEMOS      "/usr/share/demos"

#define PATH_ETC        "/etc"
#define PATH_TMP        "/tmp"
#define PATH_VAR_LOG    "/var/log"
#define PATH_DEV        "/dev"

/* Find `name` among the program directories and write its full path to `out`.
 * Returns 0, or -1 if there is no such program. An argument containing a slash
 * is a path already and is passed through, which is what lets ./thing work. */
int path_find_program(const char* name, char* out, int max);

#endif /* _PATHS_H */
