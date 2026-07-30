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
static uint32_t     g_pixel_gen[WS_MAX_WINDOWS];
static uint32_t     g_seen_resize[WS_MAX_WINDOWS];
/* Whether this window has been told the server is gone, so it is told once. */
static int          g_server_lost[WS_MAX_WINDOWS];

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
     * then writes the magic - and clears it again on the way out. */
    return __atomic_load_n(&g_mapped->magic, __ATOMIC_ACQUIRE) == WS_MAGIC
         ? g_mapped : 0;
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
    const int pixel_id = shm_open(WS_PIXEL_KEY(slot, 0), bytes, 0);
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
    g_pixel_gen[slot] = 0;
    g_seen_resize[slot] = 0;

    w->owner_pid = (uint32_t)getpid();
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->head = 0;
    w->tail = 0;
    w->present = 0;
    w->drawn = 0;
    w->req_width = width;
    w->req_height = height;
    w->resize_seq = 0;
    w->pixels_gen = 0;
    w->min_width = 64;
    w->min_height = 32;
    w->flags = 0;
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

/* Answer a resize the server asked for.
 *
 * Shared memory has no realloc, so this is an allocate-and-swap: a fresh
 * segment under the next generation's key, published only once it is in place,
 * and the old one released afterwards. The server keeps drawing from the old
 * pages throughout - it is holding its own reference to them - and switches
 * over when it notices the generation move.
 *
 * The new buffer starts blank rather than carrying the old contents across.
 * Rescaling is the client's business, and every client this system has would
 * rather redraw than have the server guess. */
static int apply_resize(int id, struct ws_window* w, struct win_event* out)
{
    unsigned width = w->req_width, height = w->req_height;
    if (width < w->min_width)   width = w->min_width;
    if (height < w->min_height) height = w->min_height;
    if (width == 0 || height == 0)
        return 0;

    const uint32_t gen = g_pixel_gen[id] + 1;
    const unsigned long bytes = (unsigned long)width * height * 4;
    const int fresh_id = shm_open(WS_PIXEL_KEY(id, gen), bytes, 0);
    if (fresh_id < 0)
        return 0;                       /* keep the old one; try again later */
    uint32_t* fresh = (uint32_t*)shm_map(fresh_id);
    if (fresh == 0) {
        shm_destroy(fresh_id);
        return 0;
    }
    for (unsigned long i = 0; i < (unsigned long)width * height; ++i)
        fresh[i] = 0xFFFFFF;

    /* Dimensions before the generation: the server reads them the other way
     * round, so it can never see a new generation described by an old size. */
    w->width = width;
    w->height = height;
    __atomic_store_n(&w->pixels_gen, gen, __ATOMIC_RELEASE);

    uint32_t* old = g_pixels[id];
    const unsigned long old_bytes = g_pixel_bytes[id];
    const int old_id = g_pixel_id[id];

    g_pixels[id] = fresh;
    g_pixel_id[id] = fresh_id;
    g_pixel_bytes[id] = bytes;
    g_pixel_gen[id] = gen;

    if (old != 0) {
        munmap(old, old_bytes);
        shm_destroy(old_id);            /* the server's reference keeps it alive */
    }

    out->type = WIN_EVENT_RESIZE;
    out->window = (uint32_t)id;
    out->x = (int32_t)width;
    out->y = (int32_t)height;
    out->button = 0;
    out->key = 0;
    return 1;
}

int win_poll(int id, struct win_event* out)
{
    if (id < 0 || id >= WS_MAX_WINDOWS)
        return 0;

    struct ws_shared* block = control();
    if (block == 0) {
        /* The server has gone - it clears the magic on its way out, and a
         * server that crashed leaves a block that never had it. Either way this
         * window can never be drawn or clicked again, so say so once rather
         * than leaving the client polling an empty queue for ever. Without this
         * a desktop shutting down leaves its clients spinning, alive, and
         * unreachable: nothing is left to send them a close. */
        if (g_pixels[id] != 0 && !g_server_lost[id]) {
            g_server_lost[id] = 1;
            out->type = WIN_EVENT_CLOSE;
            out->window = (uint32_t)id;
            out->x = 0; out->y = 0;
            out->button = 0; out->key = 0;
            return 1;
        }
        return 0;
    }
    struct ws_window* w = &block->windows[id];

    /* Ahead of the queue: a client that is told it grew before it is told
     * anything else cannot draw into the old buffer by mistake. */
    const uint32_t resize_seq = __atomic_load_n(&w->resize_seq, __ATOMIC_ACQUIRE);
    if (resize_seq != g_seen_resize[id]) {
        g_seen_resize[id] = resize_seq;
        if (apply_resize(id, w, out))
            return 1;
    }

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
        g_pixel_gen[id] = 0;
        g_server_lost[id] = 0;
    }
}

void win_size(int id, unsigned* width, unsigned* height)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS)
        return;
    if (width != 0)  *width = block->windows[id].width;
    if (height != 0) *height = block->windows[id].height;
}

void win_set_min_size(int id, unsigned width, unsigned height)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS)
        return;
    block->windows[id].min_width = width;
    block->windows[id].min_height = height;
}

void win_set_desktop(int id)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS)
        return;
    block->windows[id].flags |= WS_FLAG_DESKTOP;
}

/* --- dragging things between windows ------------------------------------- */

void win_drag_begin(const char* path, const char* label, unsigned icon,
                    int grab_x, int grab_y, int home_x, int home_y)
{
    struct ws_shared* block = control();
    if (block == 0 || path == 0)
        return;
    struct ws_drag* d = &block->drag;

    /* Everything before the phase, and the phase last: the server reads phase
     * first and would otherwise draw a ghost whose path is still the previous
     * one's. */
    int i = 0;
    for (; path[i] != '\0' && i < (int)sizeof(d->path) - 1; ++i)
        d->path[i] = path[i];
    d->path[i] = '\0';
    i = 0;
    if (label != 0)
        for (; label[i] != '\0' && i < (int)sizeof(d->label) - 1; ++i)
            d->label[i] = label[i];
    d->label[i] = '\0';

    d->icon   = icon;
    d->grab_x = grab_x;
    d->grab_y = grab_y;
    d->home_x = home_x;
    d->home_y = home_y;
    d->x      = home_x;
    d->y      = home_y;
    d->step   = 0;
    d->steps  = 0;
    __atomic_add_fetch(&d->seq, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&d->phase, WS_DRAG_LIVE, __ATOMIC_RELEASE);
}

const char* win_drag_path(void)
{
    struct ws_shared* block = control();
    if (block == 0 || __atomic_load_n(&block->drag.phase, __ATOMIC_ACQUIRE) !=
                          WS_DRAG_LIVE)
        return "";
    return (const char*)block->drag.path;
}

const char* win_drop_path(void)
{
    struct ws_shared* block = control();
    return block == 0 ? "" : (const char*)block->drag.path;
}

int win_dragging(void)
{
    struct ws_shared* block = control();
    return block != 0 &&
           __atomic_load_n(&block->drag.phase, __ATOMIC_ACQUIRE) == WS_DRAG_LIVE;
}

/* The snap is short on purpose: long enough to see where the thing went, short
 * enough that it is never in the way of the next action. The server advances
 * one step per frame and a frame is 10 ms, so this is about a sixth of a
 * second - the same order as a window opening. */
#define DRAG_SNAP_STEPS 16

static void drag_snap_to(int x, int y)
{
    struct ws_shared* block = control();
    if (block == 0)
        return;
    struct ws_drag* d = &block->drag;
    d->to_x  = x;
    d->to_y  = y;
    d->step  = 0;
    d->steps = DRAG_SNAP_STEPS;
    __atomic_store_n(&d->phase, WS_DRAG_SNAP, __ATOMIC_RELEASE);
}

void win_drop_accept(int screen_x, int screen_y) { drag_snap_to(screen_x, screen_y); }

void win_drop_reject(void)
{
    struct ws_shared* block = control();
    if (block == 0)
        return;
    drag_snap_to(block->drag.home_x, block->drag.home_y);
}

void win_origin(int id, int* x, int* y)
{
    struct ws_shared* block = control();
    if (block == 0 || id < 0 || id >= WS_MAX_WINDOWS) {
        if (x != 0) *x = 0;
        if (y != 0) *y = 0;
        return;
    }
    /* The frame's top-left plus the chrome, so this is where the content
     * begins - which is what a client's own coordinates are relative to. */
    const int chrome = (block->windows[id].flags & WS_FLAG_DESKTOP) ? 0 : 1;
    if (x != 0) *x = block->windows[id].x + (chrome ? WS_BORDER : 0);
    if (y != 0)
        *y = block->windows[id].y +
             (chrome ? WS_BORDER + WS_TITLE_HEIGHT : 0);
}
