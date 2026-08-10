/* syncd - commit the filesystem's held-back changes on a timer.
 *
 * The filesystem server batches metadata into a journal transaction and
 * commits it when the batch fills, when somebody calls sync, or when it is
 * more than a second old and another request arrives. That last condition has
 * a hole in it, and the hole is the interesting part: it needs another
 * request. A burst of writes followed by an idle filesystem leaves the batch
 * sitting there, because the server is asleep in ipc_recv with nothing to wake
 * it and no clock of its own to wake it with.
 *
 * This is that clock. It could have been a thread inside the server, but the
 * server is single-threaded on purpose - one request at a time, no locking
 * around the transaction buffer, the mount table or the block cache - and
 * putting a second thread in there to touch all three would be a large change
 * to make a small guarantee. From outside it is a client like any other, and
 * the guarantee is the same.
 *
 * This is what `update` did on early UNIX, for the same reason, and the five
 * second interval is the one ext4 uses for the same trade: a crash loses at
 * most that much, and an idle machine still writes its disk once in a while
 * rather than never.
 */

#include <unistd.h>

#define INTERVAL_MS 5000

int main(void)
{
    for (;;) {
        msleep(INTERVAL_MS);
        /* Costs one message when there is nothing held back: the server sees
         * an empty batch and returns without touching the disk. */
        sync();
    }
}
