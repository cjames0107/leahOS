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

/* Application bundles.
 *
 * This was /opt, on the reading that the FHS reserves it for add-on software
 * packages each in its own directory - which a .app technically is. But /opt
 * is where a system administrator puts third-party software; these are the
 * system's own applications, and the directory a person opens to see what
 * they can run should be named for what it holds rather than for the standard
 * that has a slot the shape of it.
 *
 * /opt is still created, still empty, and still means what it always did. */
#define PATH_APPS       "/Applications"

/* The compiled time zones, in the format libc reads. /etc/localtime is a copy
 * of one of these; /etc/timezone holds the name it was copied from, because a
 * copy does not remember where it came from. */
#define PATH_ZONEINFO   "/usr/share/zoneinfo"

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
