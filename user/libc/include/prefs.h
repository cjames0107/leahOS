#ifndef _PREFS_H
#define _PREFS_H

/* Per-user preferences, as "key value" lines.
 *
 * A text file rather than a binary blob: it can be read with cat and repaired
 * with the editor, which on a system this size is worth more than compactness.
 * Unknown keys are preserved on write, so an older program cannot silently
 * discard a newer one's settings.
 *
 * One file per application, under ~/.config, and one shared file for what the
 * desktop itself is - the theme, which is not any application's.
 *
 * It was a single ~/.leahrc that every program would have written into, and
 * exactly one program did: thirty-two keys and no namespace is a file where
 * the second application to want a setting called "size" breaks the first. So
 * a scope is chosen before the keys are, and an application that never chooses
 * one gets its own by name.
 */

/* Which file these calls read and write. `name` is a bare word - the
 * application's own name is the usual answer, and PREFS_DESKTOP is the shared
 * one. Choosing a different scope loads it; anything unsaved in the old one is
 * written back first. */
#define PREFS_DESKTOP "desktop"
void prefs_scope(const char* name);

/* Load the calling user's file. Safe to call when it does not exist. */
void prefs_load(void);

/* Write it back. Returns 0, or -1. */
int  prefs_save(void);

unsigned prefs_get_u32(const char* key, unsigned fallback);
void     prefs_set_u32(const char* key, unsigned value);
const char* prefs_get_str(const char* key, const char* fallback);
void        prefs_set_str(const char* key, const char* value);

#endif /* _PREFS_H */
