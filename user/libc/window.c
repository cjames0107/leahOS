/* The client half of the window protocol.
 *
 * Everything here is reads and writes to shared memory - there is no window
 * syscall any more. What used to be a call into the kernel is now a store into
 * a structure the server is also looking at.
 */

#include <shm.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <window.h>

/* Mapped once, whether or not the server has published itself yet. Keeping
 * these apart matters: callers poll for the server, and re-mapping the segment
 * on every failed poll would map it thousands of times and exhaust the
 * caller's address space long before the server ever appeared. */
static struct ws_shared* g_mapped = 0;
static uint32_t*    g_pixels[WS_MAX_WINDOWS];
static int          g_pixel_id[WS_MAX_WINDOWS];
static unsigned long g_pixel_bytes[WS_MAX_WINDOWS];

/* Map the control block, once. It is created by the server, so failing to find
 * it simply means the desktop is not running. */
static struct ws_shared* control(void)
{
    if (g_mapped == 0) {
        const int id = shm_open(WS_CONTROL_KEY, 0, 0);
        if (id < 0)
            return 0;
        g_mapped = (struct ws_shared*)shm_map(id);
        if (g_mapped == 0)
            return 0;
    }
    /* Mapped is not the same as ready: the server zeroes the block and only
     * then writes the magic. */
    return g_mapped->magic == WS_MAGIC ? g_mapped : 0;
}

int win_server_running(void) { return control() != 0; }

int win_create(int x, int y, unsigned width, unsigned height, const char* title)
{
    struct ws_shared* block = control();
    if (block == 0 || width == 0 || height == 0)
        return -1;

    /* Claim a slot. Several clients can be starting at once - login launches
     * three of them together - so the claim is a compare-and-swap rather than a
     * lock: exactly one caller can move a slot out of FREE. */
    int slot = -1;
    for (unsigned i = 0; i < WS_MAX_WINDOWS; ++i) {
        uint32_t expected = WS_SLOT_FREE;
        if (__atomic_compare_exchange_n(&block->windows[i].state, &expected,
                                        WS_SLOT_CLAIMED, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    struct ws_window* w = &block->windows[slot];

    /* The pixels are this client's own segment, keyed by the slot so the server
     * knows where to find them. Owned by this user, so another user cannot map
     * them even though the control block is public. */
    const unsigned long bytes = (unsigned long)width * height * 4;
    const int pixel_id = shm_open(WS_PIXEL_KEY_BASE + (unsigned)slot, bytes, 0);
    if (pixel_id < 0) {
        __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);
        return -1;
    }
    uint32_t* pixels = (uint32_t*)shm_map(pixel_id);
    if (pixels == 0) {
        __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);
        return -1;
    }
    for (unsigned long i = 0; i < (unsigned long)width * height; ++i)
        pixels[i] = 0xFFFFFF;
    g_pixels[slot] = pixels;
    g_pixel_id[slot] = pixel_id;
    g_pixel_bytes[slot] = bytes;

    w->owner_pid = (uint32_t)getpid();
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->head = 0;
    w->tail = 0;
    w->present = 0;
    w->drawn = 0;
    unsigned n = 0;
    while (title != 0 && title[n] != '\0' && n < WS_TITLE_LEN - 1) {
        w->title[n] = title[n];
        ++n;
    }
    w->title[n] = '\0';

    /* Published last: everything above must be visible before the server is
     * allowed to look at the slot. */
    __atomic_store_n(&w->state, WS_SLOT_LIVE, __ATOMIC_RELEASE);
    return slot;
}

uint32_t* win_map(int id)
{
    if (id < 0 || id >= WS_MAX_WINDOWS)
        return 0;
    return g_pixels[id];        /* mapped by win_create; this is just the handle */
}

void win_present(int id)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS)
        return;
    __atomic_add_fetch(&block->windows[id].present, 1, __ATOMIC_RELEASE);
}

int win_poll(int id, struct win_event* out)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS)
        return 0;
    struct ws_window* w = &block->windows[id];

    const uint32_t head = __atomic_load_n(&w->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = w->tail;
    if (tail == head)
        return 0;
    *out = w->events[tail % WS_EVENT_SLOTS];
    __atomic_store_n(&w->tail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}

void win_destroy(int id)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS)
        return;

    /* Release the slot first, so the server stops drawing from these pixels
     * before they are given up. */
    __atomic_store_n(&block->windows[id].state, WS_SLOT_FREE, __ATOMIC_RELEASE);

    /* Then let the segment go. The server may still have it mapped; its own
     * reference keeps the pages alive until it unmaps them. Dropping the key is
     * what lets the next window in this slot get a segment of its own size
     * rather than inheriting this one's. */
    if (g_pixels[id] != 0) {
        munmap(g_pixels[id], g_pixel_bytes[id]);
        shm_destroy(g_pixel_id[id]);
        g_pixels[id] = 0;
        g_pixel_id[id] = -1;
        g_pixel_bytes[id] = 0;
    }
}
