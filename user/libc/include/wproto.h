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

/* The chrome, which both sides have to agree about: the server draws it and a
 * client needs it to turn its own coordinates into screen ones. */
/* Tall enough for three controls and a line of proportional text with air
 * around it. The old eighteen was sized for an 8x16 bitmap font and a close
 * box, and nothing about it survives that font going. */
#define WS_TITLE_HEIGHT 28
/* A hairline. The frame used to be a hard outline with a bevel inside it, and
 * three pixels of grey was that bevel; the depth now comes from a shadow
 * outside the window rather than from anything drawn in its edge. */
#define WS_BORDER       1

/* Every window, panel and control is rounded by the same amount. One radius
 * for the whole interface is what makes it read as one interface. */
#define WS_CORNER       8

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

/* Something was dropped on this window. x and y are where, in the window's own
 * coordinates; what was dropped is in the control block's drag record, which
 * the server leaves intact until the next drag begins.
 *
 * The receiving client decides. It either does the move and calls
 * win_drop_accept with where the thing ended up on screen, or calls
 * win_drop_reject - and the server animates the ghost to that place or back to
 * where it was picked up. Nothing moves on screen until somebody says what
 * happened, which is what makes a refused drop visibly a refusal rather than
 * an icon quietly vanishing. */
#define WIN_EVENT_DROP       7

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
    /* What was held when this happened. Only meaningful for a press: a key
     * event's modifiers are already folded into `key`. WIN_MOD_* below. */
    uint32_t modifiers;
};

#define WIN_MOD_SHIFT 1u
#define WIN_MOD_CTRL  2u

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
/* A drag in progress.
 *
 * It lives in the server because it is the one thing that has to be drawn over
 * every window at once, including windows belonging to other processes. A
 * client that started a drag cannot paint outside itself, and the two clients
 * either end of a drag do not know about each other at all - they only both
 * know the server.
 *
 * The payload is a path. That is the only kind of thing this system drags, and
 * a general type tag with nothing to distinguish would be a guess about a
 * second case that does not exist yet.
 */
#define WS_DRAG_NONE  0u
#define WS_DRAG_LIVE  1u   /* following the cursor                     */
#define WS_DRAG_SNAP  2u   /* let go: sliding to where it ended up     */

/* What to draw for the ghost. The server cannot read a file to find out what it
 * is, and asking it to would put the filesystem in the compositor. */
#define WS_DRAG_FILE   0u
#define WS_DRAG_FOLDER 1u
#define WS_DRAG_APP    2u

#define WS_DRAG_W 64
#define WS_DRAG_H 46

struct ws_drag {
    volatile uint32_t phase;        /* WS_DRAG_*                              */
    volatile uint32_t source;       /* the slot that started it               */
    volatile uint32_t icon;         /* WS_DRAG_FILE / FOLDER / APP            */
    volatile int32_t  x, y;         /* the ghost's top-left, in screen pixels */
    volatile int32_t  grab_x, grab_y;   /* cursor offset inside the ghost     */
    volatile int32_t  home_x, home_y;   /* where it was picked up             */
    volatile int32_t  to_x, to_y;       /* where the snap is heading          */
    volatile uint32_t step, steps;      /* how far through the snap           */
    /* Bumped whenever a drag starts. A client redrawing itself uses this to
     * notice that the thing it is holding has changed without polling paths. */
    volatile uint32_t seq;
    char path[256];                 /* what is being dragged                  */
    char label[64];                 /* and what to write under the ghost      */
};

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

    /* Read by clients as well as by the server: a selection highlight drawn a
     * different colour in every application is not a theme. */
    volatile uint32_t selection;    /* the highlight behind a chosen item     */
    volatile uint32_t body;         /* a window's content background          */
    volatile uint32_t text;         /* ink on that background                 */
    volatile uint32_t text_scale;   /* 1 or 2 - the font is a bitmap          */
    volatile int32_t  contrast;     /* -100..100, applied to the bevels       */
    volatile uint32_t pattern;      /* WS_PATTERN_*, drawn when no wallpaper  */

    /* Whether a window's backdrop is frosted glass or a flat opaque panel.
     *
     * Blurring is by a wide margin the most expensive thing the compositor
     * does - about a thousand milliseconds per megapixel in the guest against
     * ten for a copy - so on a machine without acceleration it is the first
     * thing worth turning off. Off is the default for that reason: the glass
     * is what the interface wants to be, not what it has to be. */
    volatile uint32_t blur;         /* 1 frosted, 0 opaque                    */
};

/* Backdrop patterns. Drawn from the desktop colour rather than a second one, so
 * a pattern stays consistent with whatever colour was chosen. */
#define WS_PATTERN_FLAT   0
#define WS_PATTERN_GRID   1
#define WS_PATTERN_DOTS   2
#define WS_PATTERN_WEAVE  3
/* Alternate pixels, which at this distance reads as a shade between the two
 * and is what the desktop of this era actually was - a one-bit machine making
 * a grey it did not have. */
#define WS_PATTERN_DITHER 4
#define WS_PATTERN_COUNT  5

struct ws_shared {
    volatile uint32_t magic;
    volatile uint32_t server_pid;
    volatile uint32_t quit;         /* set by the server as it exits */
    volatile uint32_t reserved;
    struct ws_theme theme;
    struct ws_drag  drag;
    struct ws_window windows[WS_MAX_WINDOWS];
};

#endif /* _WPROTO_H */
