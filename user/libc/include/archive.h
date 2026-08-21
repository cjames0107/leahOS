#ifndef _ARCHIVE_H
#define _ARCHIVE_H

/* tar archives, read and written.
 *
 * The format is deliberately dull: a 512-byte header, the file's bytes padded
 * up to the next 512, and so on, ending with two zeroed blocks. Numbers are
 * octal in fixed-width fields, which is what a format designed to be written
 * by a program with no library looks like.
 *
 * It lives here rather than in the one program that reads them because there
 * are two of those now - the command and the application - and a file format
 * described twice is a file format that will eventually be described
 * differently in the two places.
 *
 * Archives are handled whole in memory. A tar is read start to finish anyway
 * (there is no index; finding the last member means walking every one before
 * it), and holding it means a gzipped archive can be inflated once and then
 * walked exactly like a plain one.
 */

#include <stdint.h>

#define AR_FILE  0
#define AR_DIR   1
#define AR_OTHER 2      /* a symlink, a device: named and passed over */

struct ar_entry {
    char          path[256];
    unsigned long size;
    unsigned      mode;
    int           kind;
    unsigned long at;   /* where the body starts, within the archive */
};

/* Read a whole archive, inflating it if it is gzipped. The caller frees what
 * comes back. Returns 0 and leaves errno set on failure. */
unsigned char* ar_read(const char* path, unsigned long* len);

/* Every member, in order. The visitor is given the entry and a pointer to its
 * body inside `data`; returning non-zero stops the walk. Returns how many
 * members were seen, or -1 if it is not a tar at all. */
typedef int (*ar_visit)(void* user, const struct ar_entry* e,
                        const unsigned char* body);
long ar_walk(const unsigned char* data, unsigned long len,
             ar_visit visit, void* user);

/* Put one member on disk, under `into`. Refuses a path that would escape it -
 * an archive is not allowed to choose where it lands. 0 on success. */
int ar_extract(const struct ar_entry* e, const unsigned char* body,
               const char* into);

/* --- writing ---------------------------------------------------------------
 *
 * Streamed rather than built: a member is written as it is added, so making an
 * archive of a directory costs one file's worth of memory rather than the
 * whole tree's. */
struct ar_out;

struct ar_out* ar_create(const char* path);

/* Add one file or directory, stored under the name `as`. */
int ar_add(struct ar_out* a, const char* on_disk, const char* as);

/* Add a directory and everything under it, stored under `as`. */
int ar_add_tree(struct ar_out* a, const char* dir, const char* as);

/* Write the end-of-archive blocks and close. Returns 0, or -1 if anything
 * along the way failed. Frees `a` either way. */
int ar_finish(struct ar_out* a);

#endif /* _ARCHIVE_H */
