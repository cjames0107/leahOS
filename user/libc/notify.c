/* See <notify.h>. */

#include <notify.h>
#include <shm.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wproto.h>

static struct ws_shared* g_block;

static struct ws_shared* block(void)
{
    if (g_block == 0) {
        const int id = shm_open(WS_CONTROL_KEY, 0, 0);
        if (id >= 0)
            g_block = (struct ws_shared*)shm_map(id);
    }
    return (g_block != 0 && g_block->magic == WS_MAGIC) ? g_block : 0;
}

static void copy_into(char* out, unsigned long max, const char* in)
{
    unsigned long n = 0;
    if (in != 0)
        while (in[n] != '\0' && n + 1 < max) { out[n] = in[n]; ++n; }
    out[n] = '\0';
}

void notify(const char* from, const char* text)
{
    struct ws_shared* b = block();
    if (b == 0 || text == 0 || text[0] == '\0')
        return;

    /* The sequence is claimed first, so two programs posting at once get a
     * slot each rather than the same one. */
    const uint32_t seq = __atomic_add_fetch(&b->notes.next, 1, __ATOMIC_ACQ_REL);
    struct ws_note* n = &b->notes.ring[(seq - 1) % WS_NOTES_MAX];

    /* Cleared before it is filled, so a reader that arrives mid-write sees a
     * sequence that is not yet the new one and skips the slot rather than
     * reading half of the last message and half of this one. */
    __atomic_store_n(&n->seq, 0, __ATOMIC_RELEASE);
    copy_into(n->from, sizeof(n->from), from);
    copy_into(n->text, sizeof(n->text), text);
    n->at_ms = (uint32_t)uptime_ms();
    /* Published last: everything above it is in place by the time a reader can
     * tell there is anything to read. */
    __atomic_store_n(&n->seq, seq, __ATOMIC_RELEASE);
}
