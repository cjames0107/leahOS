#ifndef _WPROTO_H
#define _WPROTO_H

/* The window protocol: what the server and its clients agree on.
 *
 * There is no message passing here. The whole protocol is one shared memory
 * segment that both sides map and poke at, which is the least machinery that
 * gets a window server out of the kernel: no transport to write, no
 * marshalling, and the pixels are shared rather than copied.
 *
 * The rules that keep that honest:
 *
 *  - A client owns its slot and writes only geometry, title and `present`.
 *  - The server owns the screen and the event rings, and writes only those.
 *  - Slot allocation is the one genuinely contended step, so it goes through a
 *    compare-and-swap on `state` rather than a lock. Whoever wins the swap owns
 *    the slot; everyone else moves on to the next one.
 *
 * The control block is deliberately public - every user can map it - because it
 * is the rendezvous and a client is not root. A window's pixels are not: they
 * stay owned by the client that made them, so one user's windows cannot be read
 * by another. See the note in the README about what that does and does not buy.
 */

#include <stdint.h>

#define WS_CONTROL_KEY    1u          /* shm key of the control block      */
#define WS_PIXEL_KEY_BASE 0x1000u     /* base of the per-window pixel keys  */

/* The key of a window's pixels. It carries a generation as well as a slot,
 * because a resize replaces the segment rather than growing it - there is no
 * realloc for shared memory - and the old and the new have to be able to exist
 * at the same time. If both used one key the server would be unable to tell
 * which of the two it had just opened, and would read the old segment's pages
 * through the new segment's dimensions. */
#define WS_PIXEL_GENS 64u
#define WS_PIXEL_KEY(slot, gen) \
    (WS_PIXEL_KEY_BASE + (unsigned)(slot) * WS_PIXEL_GENS + ((gen) % WS_PIXEL_GENS))
#define WS_MAGIC          0x5734454cu /* "L4Ws" - the server is up          */

#define WS_MAX_WINDOWS 16
#define WS_EVENT_SLOTS 32
#define WS_TITLE_LEN   32

/* Window flags, set by the client before the slot goes live.
 *
 * WS_FLAG_DESKTOP is what makes desktop icons possible without teaching the
 * server about them: the window is drawn without chrome, never raises above
 * anything, and cannot be dragged or resized. It is a place for a client to
 * draw on rather than a window in the usual sense, and everything else about it
 * - its pixels, its events - works exactly as any other window does. */
#define WS_FLAG_DESKTOP 1u

/* Slot states. A client walks 0 -> CLAIMED -> LIVE and finally back to FREE. */
#define WS_SLOT_FREE    0u
#define WS_SLOT_CLAIMED 1u   /* won by a client, not yet filled in           */
#define WS_SLOT_LIVE    2u   /* geometry and pixels valid, server may draw   */

/* Event types, as delivered to a client. */
#define WIN_EVENT_NONE       0
#define WIN_EVENT_MOUSE_DOWN 1
#define WIN_EVENT_MOUSE_UP   2
#define WIN_EVENT_MOUSE_MOVE 3
#define WIN_EVENT_KEY        4
#define WIN_EVENT_CLOSE      5   /* the close box was clicked */
/* Close the focused window: Ctrl+Q, which the console driver delivers as the
 * control character 0x11.
 *
 * The *server* owns this, not its clients. A plain letter cannot be a quit key
 * - a terminal or a text field has every right to it - and having each client
 * invent its own shortcut would mean no two windows closed the same way. So the
 * window manager translates the chord into the same WIN_EVENT_CLOSE the close
 * box sends, and a client needs no quit handling at all beyond the one it
 * already has. */
#define WIN_KEY_CLOSE 0x11

/* The arrows, as the console driver delivers them - control characters chosen
 * from the range Ctrl+letter does not occupy, so a key event stays one byte. */
#define WIN_KEY_UP    0x1C
#define WIN_KEY_DOWN  0x1D
#define WIN_KEY_LEFT  0x1E
#define WIN_KEY_RIGHT 0x1F

/* The window has been resized. x and y carry the new content width and height;
 * the pixel buffer has already been replaced, so re-fetch it with win_map and
 * redraw everything - the new one starts blank. */
#define WIN_EVENT_RESIZE     6

struct win_event {
    uint32_t type;
    uint32_t window;
    int32_t  x, y;      /* relative to the window's content area */
    uint32_t button;    /* 1 left, 2 right - a right press is delivered as a
                           MOUSE_DOWN and is the client's cue to raise a
                           context menu; the server does not raise or focus the
                           window for one */
    uint32_t key;       /* the character, for key events */
};

struct ws_window {
    volatile uint32_t state;
    volatile uint32_t owner_pid;
    volatile int32_t  x, y;         /* top-left of the frame, owned by the server
                                       once live: dragging moves it */
    volatile uint32_t width, height;/* the content area, owned by the client   */
    volatile uint32_t present;      /* client bumps it after drawing           */
    volatile uint32_t drawn;        /* server copies present here once shown   */
    char title[WS_TITLE_LEN];

    /* Resizing. The server asks and the client answers, because the pixels are
     * the client's to allocate: the server writes a requested size and bumps
     * resize_seq, and the client replaces its segment and bumps pixels_gen.
     * Neither side ever changes the other's fields, so no lock is needed - only
     * the ordering, which is why each generation counter is stored last. */
    volatile uint32_t req_width, req_height;
    volatile uint32_t resize_seq;   /* server bumps to request a size          */
    volatile uint32_t pixels_gen;   /* client bumps once the new segment is up  */
    volatile uint32_t min_width, min_height;  /* the client's floor            */
    volatile uint32_t flags;

    /* Event ring. The server writes at head, the client reads at tail. One
     * writer and one reader, so no lock is needed - only the ordering of the
     * two stores, which is why head moves last. */
    volatile uint32_t head, tail;
    struct win_event events[WS_EVENT_SLOTS];
};

/* The desktop's appearance, owned by whoever is allowed to change it and read
 * by the server every pass.
 *
 * It lives in the control block rather than in a file because the server is the
 * only thing that can act on it, and a setting nothing acts on is decoration.
 * `generation` is bumped by the writer; the server reloads the wallpaper when
 * it moves, so a colour change costs a repaint and a wallpaper change costs a
 * decode - and only when one actually happened. */
struct ws_theme {
    volatile uint32_t desktop;
    volatile uint32_t face;
    volatile uint32_t light;
    volatile uint32_t shadow;
    volatile uint32_t title_active;
    volatile uint32_t title_idle;
    volatile uint32_t title_text;
    volatile uint32_t cursor;       /* the arrow's fill; its outline stays black */
    volatile uint32_t generation;
    char wallpaper[128];            /* a PNG to show behind the windows, or "" */
};

struct ws_shared {
    volatile uint32_t magic;
    volatile uint32_t server_pid;
    volatile uint32_t quit;         /* set by the server as it exits */
    volatile uint32_t reserved;
    struct ws_theme theme;
    struct ws_window windows[WS_MAX_WINDOWS];
};

#endif /* _WPROTO_H */
