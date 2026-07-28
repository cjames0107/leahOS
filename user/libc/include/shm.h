#ifndef _SHM_H
#define _SHM_H

#include <stdint.h>

/* Creation flags. */
#define SHM_PUBLIC 1u   /* mappable by every user, not just the creator and root */

/* Shared memory between processes.
 *
 * A segment is named by a number rather than a path - there is no /dev to hang
 * a name off - so two processes that agree on a key find the same pages. The
 * first process to ask for a key creates the segment; later ones get what is
 * already there and the size argument is ignored.
 *
 * A segment belongs to the user that created it, and only that user and root
 * can map it, unless it was created with SHM_PUBLIC. */

/* Returns a segment id, or -1. */
int shm_open(unsigned key, unsigned long bytes, unsigned flags);

/* Map a segment into this process. Returns its address, or 0. */
void* shm_map(int id);

unsigned long shm_size(int id);

/* Give up this process's claim on a segment, so its key can be reused. Anyone
 * who still has it mapped keeps it alive until they let go. */
int shm_destroy(int id);

#endif /* _SHM_H */
