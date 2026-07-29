#ifndef _CLIPBOARD_H
#define _CLIPBOARD_H

/* The system clipboard.
 *
 * A shared memory segment under a well-known key, exactly like the window
 * server's control block - which is the only cross-process channel this system
 * has, and the right one: a clipboard *is* a piece of shared state that outlives
 * whoever put something in it.
 *
 * It is public, so any user can read it. That is the same trade the control
 * block makes and it is worth naming: on a machine with two people logged in
 * one could read the other's copied text. Fixing it needs a per-session
 * clipboard, which needs a session the system does not yet have.
 */

#define CLIP_KEY  2u
#define CLIP_MAX  4096

/* Put text on the clipboard. Returns 0, or -1 if there is no clipboard. */
int clip_put(const char* text, unsigned length);

/* Copy the clipboard into `out`, NUL-terminated. Returns the length, or -1. */
int clip_get(char* out, unsigned max);

/* Bumped whenever something is put, so a window can notice a change. */
unsigned clip_generation(void);

#endif /* _CLIPBOARD_H */
