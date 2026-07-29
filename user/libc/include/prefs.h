#ifndef _PREFS_H
#define _PREFS_H

/* Per-user preferences, kept in ~/.leahrc as "key value" lines.
 *
 * A text file rather than a binary blob: it can be read with cat and repaired
 * with the editor, which on a system this size is worth more than compactness.
 * Unknown keys are preserved on write, so an older program cannot silently
 * discard a newer one's settings.
 */

/* Load the calling user's file. Safe to call when it does not exist. */
void prefs_load(void);

/* Write it back. Returns 0, or -1. */
int  prefs_save(void);

unsigned prefs_get_u32(const char* key, unsigned fallback);
void     prefs_set_u32(const char* key, unsigned value);
const char* prefs_get_str(const char* key, const char* fallback);
void        prefs_set_str(const char* key, const char* value);

#endif /* _PREFS_H */
