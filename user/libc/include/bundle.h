#ifndef _BUNDLE_H
#define _BUNDLE_H

/* Application bundles: a directory that is an application.
 *
 * A ".app" is an ordinary directory whose name carries the extension, holding
 * the program and everything the system needs to know about it:
 *
 *     Paint.app/
 *         Info            what this is - see below
 *         paint.elf       the program
 *         Icon.png        optional, 32x32
 *
 * The point is that an application stops being "a binary in /BIN plus rules
 * scattered through whoever launches it". A file browser needed a hardcoded
 * list of which programs open which documents, and a context menu had to be
 * written into every client; both of those belong to the application, and now
 * live with it. Nothing outside reads a bundle except through this header.
 *
 * Info is the same "key value" line format as ~/.leahrc, for the same reason:
 * it can be read with cat and repaired with the editor.
 *
 *     name    Paint
 *     exec    paint.elf
 *     icon    Icon.png
 *     opens   .png .gif
 *     menu    New drawing
 *     menu    Open recent
 *
 * `opens` is how a document finds its application without the browser knowing
 * anything about file types. `menu` lines become the application's own entries
 * in a right-click menu, which is how a bundle extends the shell rather than
 * the shell enumerating bundles.
 */

#define BUNDLE_MAX_MENU 6
#define BUNDLE_MAX_EXT  8

struct bundle {
    char path[192];                     /* the .app directory itself */
    char name[64];
    char exec[64];                      /* relative to the bundle */
    char icon[64];                      /* relative, or empty */
    char opens[BUNDLE_MAX_EXT][12];     /* upper-cased, with the dot */
    int  opens_n;
    char menu[BUNDLE_MAX_MENU][32];
    int  menu_n;
};

/* Whether a name ends in ".app", case-insensitively. Directories are the only
 * things this is true of in practice, but the test is on the name because that
 * is what makes a bundle recognisable without opening it. */
int bundle_is_app(const char* name);

/* Read `path`/Info. Returns 0, or -1 when it is not a usable bundle. A bundle
 * with no `exec` is not usable: there would be nothing to run. */
int bundle_load(const char* path, struct bundle* out);

/* The full path of the program to execute. */
void bundle_exec(const struct bundle* b, char* out, int max);

/* Where the system's applications live. A single place, so that "which
 * application is called Edit" has one answer rather than one per caller. */
#define BUNDLE_DIR "/Apps"

/* The bundle named `name` (its Info `name`, or its directory), or -1. This is
 * how one application launches another without knowing a path: nothing outside
 * a bundle should contain the string "/BIN/EDIT.ELF" again. */
int bundle_find(const char* name, struct bundle* out);

/* Load a bundle and hand back the command to run, in one step. Returns 0. */
int bundle_command(const char* name, char* out, int max);

/* The same, as a string that can be passed straight to execve. Returns an empty
 * string when there is no such application, which every caller here treats as
 * "then do not launch it" - a missing application should be a launch that does
 * nothing, not a launch of the wrong thing. The buffer is reused, so use it
 * before asking again. */
const char* app_path(const char* name);

/* The bundle in `dir` that claims `document`'s extension, or -1. `dir` is
 * searched for .app directories - normally /Apps. */
int bundle_for_document(const char* dir, const char* document,
                        struct bundle* out);

/* --- aliases ---------------------------------------------------------------
 *
 * There are no symbolic links in this filesystem, so a shortcut is a small text
 * file whose single line is the path it stands for, named with a ".alias"
 * extension. Opening one opens what it names.
 *
 * A file rather than a filesystem feature on purpose: a link needs the kernel
 * and the on-disk format to agree about it, and this needs neither. It is
 * readable with cat and repairable with the editor, which is the same bargain
 * Info and ~/.leahrc make.
 */
int alias_is(const char* name);

/* Read the target out of an alias. Returns 0, or -1 if it is not one or cannot
 * be read. */
int alias_target(const char* path, char* out, int max);

#endif /* _BUNDLE_H */
