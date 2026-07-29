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

/* The bundle in `dir` that claims `document`'s extension, or -1. `dir` is
 * searched for .app directories - normally /Apps. */
int bundle_for_document(const char* dir, const char* document,
                        struct bundle* out);

#endif /* _BUNDLE_H */
