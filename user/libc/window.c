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

/* What this process knows about its own windows, indexed by slot.
 *
 * Grown rather than declared: a slot number is now a position in a table that
 * spans several segments, and a fixed array here would put back exactly the
 * limit the banks removed - a client handed slot 900 would write past the end
 * of a 32-entry array, which is worse than refusing it. */
static uint32_t**    g_pixels;
static int*          g_pixel_id;
static unsigned long* g_pixel_bytes;
static uint32_t*     g_pixel_gen;
static uint32_t*     g_seen_resize;
/* Whether this window has been told the server is gone, so it is told once. */
static int*          g_server_lost;
static int           g_known;       /* how many slots those tables hold */

/* The banks, mapped as they are reached. Bank 0 is the control block's own. */
static struct ws_window* g_bank[WS_MAX_BANKS];

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

/* --- the window table ------------------------------------------------------ */

/* Make the per-slot tables reach at least `slots`. Zeroed as they grow, so a
 * slot nobody has used yet reads as unmapped rather than as whatever the
 * allocator left there. */
static int know(int slots)
{
    if (slots <= g_known)
        return 1;
    struct { void** at; unsigned long each; } table[] = {
        { (void**)&g_pixels,      sizeof(uint32_t*)     },
        { (void**)&g_pixel_id,    sizeof(int)           },
        { (void**)&g_pixel_bytes, sizeof(unsigned long) },
        { (void**)&g_pixel_gen,   sizeof(uint32_t)      },
        { (void**)&g_seen_resize, sizeof(uint32_t)      },
        { (void**)&g_server_lost, sizeof(int)           },
    };
    for (unsigned t = 0; t < sizeof(table) / sizeof(table[0]); ++t) {
        void* grown = malloc(table[t].each * (unsigned long)slots);
        if (grown == 0)
            return 0;       /* what was already there is still valid */
        memset(grown, 0, table[t].each * (unsigned long)slots);
        if (*table[t].at != 0)
            memcpy(grown, *table[t].at, table[t].each * (unsigned long)g_known);
        free(*table[t].at);
        *table[t].at = grown;
    }
    g_known = slots;
    return 1;
}

int ws_slot_count(void)
{
    const struct ws_shared* block = control();
    if (block == 0)
        return 0;
    unsigned banks = __atomic_load_n(&block->banks, __ATOMIC_ACQUIRE);
    if (banks < 1) banks = 1;
    if (banks > WS_MAX_BANKS) banks = WS_MAX_BANKS;
    return (int)banks * WS_BANK_WINDOWS;
}

struct ws_window* ws_slot(int slot)
{
    struct ws_shared* block = control();
    if (block == 0 || slot < 0 || slot >= ws_slot_count())
        return 0;
    const int bank = slot / WS_BANK_WINDOWS;
    const int at   = slot % WS_BANK_WINDOWS;
    if (bank == 0)
        return &block->windows[at];
    if (g_bank[bank] == 0) {
        /* Mapped on first use. The segment already exists - `banks` is only
         * raised once it does - so this opens rather than creates. */
        const int id = shm_open(WS_BANK_KEY(bank), sizeof(struct ws_bank),
                                SHM_PUBLIC);
        if (id < 0)
            return 0;
        g_bank[bank] = (struct ws_window*)shm_map(id);
        if (g_bank[bank] == 0)
            return 0;
    }
    return &g_bank[bank][at];
}

int ws_add_bank(void)
{
    struct ws_shared* block = control();
    if (block == 0)
        return -1;
    unsigned have = __atomic_load_n(&block->banks, __ATOMIC_ACQUIRE);
    if (have < 1) have = 1;
    if (have >= WS_MAX_BANKS)
        return -1;

    /* Made before it is announced, and announced with a compare-and-swap:
     * two clients can run out of slots at the same moment, and both are
     * allowed to make the same bank - shm_open on one key returns one
     * segment. What must not happen is `banks` going backwards. */
    const int id = shm_open(WS_BANK_KEY((int)have), sizeof(struct ws_bank),
                            SHM_PUBLIC);
    if (id < 0)
        return -1;
    struct ws_window* fresh = (struct ws_window*)shm_map(id);
    if (fresh == 0)
        return -1;
    g_bank[have] = fresh;

    unsigned seen = have;
    while (seen < have + 1 &&
           !__atomic_compare_exchange_n(&block->banks, &seen, have + 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ;
    return ws_slot_count();
}

int win_create(int x, int y, unsigned width, unsigned height, const char* title)
{
    struct ws_shared* block = control();
    if (block == 0 || width == 0 || height == 0)
        return -1;

    /* Claim a slot. Several clients can be starting at once - login launches
     * three of them together - so the claim is a compare-and-swap rather than a
     * lock: exactly one caller can move a slot out of FREE. */
    int slot = -1;
    for (int attempt = 0; attempt < 2 && slot < 0; ++attempt) {
        const int slots = ws_slot_count();
        for (int i = 0; i < slots; ++i) {
            struct ws_window* candidate = ws_slot(i);
            if (candidate == 0)
                continue;
            uint32_t expected = WS_SLOT_FREE;
            if (__atomic_compare_exchange_n(&candidate->state, &expected,
                                            WS_SLOT_CLAIMED, 0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                slot = i;
                break;
            }
        }
        /* Every slot taken: make another bank and walk it. Once - if the
         * second pass also finds nothing, another client took the new slots
         * in between, and looping on that is a way to spin forever. */
        if (slot < 0 && attempt == 0 && ws_add_bank() < 0)
            break;
    }
    if (slot < 0)
        return -1;

    struct ws_window* w = ws_slot(slot);
    /* The slot is claimed from here on, so every way out has to give it back -
     * a slot left in CLAIMED is one the server never draws and no client can
     * ever win again. */
    if (w == 0 || !know(ws_slot_count())) {
        if (w != 0)
            __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);
        return -1;
    }

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
    /* Slots are reused, so anything a previous window left here is this
     * window's problem. A text editor inheriting a settings window's sidebar
     * width had the server tinting a column across its title bar for a sidebar
     * that was not there. */
    w->sidebar = 0;
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
    if (id < 0 || id >= g_known)
        return 0;
    return g_pixels[id];        /* mapped by win_create; this is just the handle */
}

void win_present(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    __atomic_add_fetch(&w->present, 1, __ATOMIC_RELEASE);
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
    if (id < 0 || id >= g_known)
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
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return 0;

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
    struct ws_window* w = ws_slot(id);
    if (w == 0 || id >= g_known)
        return;

    /* Release the slot first, so the server stops drawing from these pixels
     * before they are given up. */
    __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);

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
    const struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    if (width != 0)  *width = w->width;
    if (height != 0) *height = w->height;
}

void win_set_min_size(int id, unsigned width, unsigned height)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    w->min_width = width;
    w->min_height = height;
}

void win_set_sidebar(int id, unsigned width)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    w->sidebar = width;
}

void win_set_alpha(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    w->flags |= WS_FLAG_ALPHA;
}

void win_hide(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    __atomic_or_fetch(&w->flags, WS_FLAG_HIDDEN, __ATOMIC_RELEASE);
}

void win_show(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    __atomic_and_fetch(&w->flags, ~WS_FLAG_HIDDEN, __ATOMIC_RELEASE);
}

/* The client draws its own title strip, and its buffer is that much taller. */
void win_set_client_title(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    w->flags |= WS_FLAG_CLIENT_TITLE;
}

/* "That press was not one of my controls - move the window instead."
 *
 * Only meaningful from a window that owns its title strip; anywhere else the
 * server is already handling the drag itself. */
void win_move_begin(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    __atomic_add_fetch(&w->move_request, 1, __ATOMIC_RELEASE);
}

/* A panel of the application's own: no chrome, and the server leaves its
 * position alone. See WS_FLAG_SHEET in wproto.h for why a dialogue has to be a
 * window rather than something drawn over one. */
void win_set_sheet(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    w->flags |= WS_FLAG_SHEET;
}

void win_set_desktop(int id)
{
    struct ws_window* w = ws_slot(id);
    if (w == 0)
        return;
    w->flags |= WS_FLAG_DESKTOP;
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
    const struct ws_window* w = ws_slot(id);
    if (w == 0) {
        if (x != 0) *x = 0;
        if (y != 0) *y = 0;
        return;
    }
    /* The frame's top-left plus the chrome, so this is where the content
     * begins - which is what a client's own coordinates are relative to. */
    const int chrome = (w->flags & WS_FLAG_DESKTOP) ? 0 : 1;
    if (x != 0) *x = w->x + (chrome ? WS_BORDER : 0);
    if (y != 0)
        *y = w->y + (chrome ? WS_BORDER + WS_TITLE_HEIGHT : 0);
}
