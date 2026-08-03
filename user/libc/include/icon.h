#ifndef _ICON_H
#define _ICON_H

#include <stdint.h>

/* The system's icons: 32x32 pictures, loaded from disk and kept.
 *
 * Icons used to be drawn - a few rectangles and a bevel, per program, each
 * with its own idea of what a folder looks like. They are files now, which
 * means they are the same everywhere and can be changed without a compiler.
 *
 * Pixels are 0xAARRGGBB and the alpha is one bit in practice: these are cut-out
 * shapes, and a pixel is either part of the icon or is not there at all. Draw
 * with wg_icon, which skips the ones that are not there rather than blending
 * them - there is nothing to blend against but whatever is already drawn.
 *
 * Every load is cached by path for the life of the process. Thirty-two by
 * thirty-two is four kilobytes; a file browser showing a folder of documents
 * asks for the same icon dozens of times, and free() is a no-op in this libc
 * anyway, so not caching would be the expensive choice twice over.
 */

#define ICON_SIZE 32

/* By name, from /share/icons: "folder-empty", "terminal", and so on. Returns 0
 * if there is no such icon, which callers should treat as "draw nothing" and
 * not as an error - a system with no icons installed should still work. */
const uint32_t* icon_by_name(const char* name);

/* By full path, for bundles, which carry their own Icon.png. */
const uint32_t* icon_by_path(const char* path);

/* What to show for a directory entry. `is_dir` and `is_app` are what the
 * caller already knows; the name decides the rest, by extension. An
 * application's own icon comes from its bundle, so `dir_path` is the directory
 * the entry lives in - pass 0 if it is not to hand, and a generic application
 * icon comes back instead. */
const uint32_t* icon_for_entry(const char* dir_path, const char* name,
                               int is_dir, int is_app);

#endif /* _ICON_H */
