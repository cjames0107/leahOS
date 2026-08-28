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
/* Base of the per-window pixel keys, and of the window banks.
 *
 * The pixel base was 0x1000, which was fine while there could only be sixteen
 * windows: the keys ran to 0x13FF and nothing else lived there. Slots are no
 * longer bounded by an array, so the range is no longer bounded either, and
 * 0x1000 + slot * 64 walks straight into the audio, block, network and
 * filesystem keys at 0x4155, 0x424C, 0x4E49 and 0x5646 - at about the two
 * hundredth window, which is now reachable. Moved somewhere with nothing above
 * it for two billion keys. */
#define WS_PIXEL_KEY_BASE 0x80000000u
#define WS_BANK_KEY_BASE  0x400u      /* base of the window-table banks     */

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

/* How many windows can exist at once: as many as the machine has room for.
 *
 * It was a fixed array. Sixteen, which an ordinary afternoon reaches - the
 * desktop, a terminal and a dozen applications - and then thirty-two, which
 * only moves the wall. Either way the failure was the worst kind: the
 * application started, could not get a slot, exited, and from where the person
 * was sitting nothing happened at all.
 *
 * So the table is a chain of banks. Bank 0 is inside the control block, which
 * is the common case and costs no extra mapping; every bank after it is a
 * public segment of its own, created by whichever client first needs a slot in
 * it. `banks` says how many exist, and it only ever grows - a bank is never
 * taken away, because a client holding a pointer into it has no way to be told.
 *
 * WS_MAX_BANKS bounds the directory, not the desktop: 64 banks is 2048
 * windows, and the kernel has 512 shared segments in total, one of which every
 * window's pixels need. The machine runs out an order of magnitude first. */
#define WS_BANK_WINDOWS 32
#define WS_BANK_KEY(b)  (WS_BANK_KEY_BASE + (unsigned)(b))
#define WS_MAX_BANKS    64
#define WS_MAX_WINDOWS  (WS_BANK_WINDOWS * WS_MAX_BANKS)
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

/* The client's pixels carry alpha in the high byte, and the server blends
 * them onto what it has already composed rather than copying over it.
 *
 * This is what lets the glass reach past the frame. A window's chrome is a
 * blurred backdrop with a wash of white on it; without this its contents were
 * an opaque rectangle laid on top, so the effect stopped at the title bar and
 * the window read as two different materials joined at a seam.
 *
 * Only windows that say so, because a client that has not thought about its
 * alpha byte leaves it zero, and zero here means invisible. */
#define WS_FLAG_ALPHA   2u

/* The client's pixels start at the top of the frame, title bar included.
 *
 * A window has two bars otherwise: the server's title bar, and whatever band
 * of controls the application puts directly beneath it. They are the same
 * height, they touch, and they are about the same thing - what this window is
 * and what you can do with it - so they read as one bar drawn twice.
 *
 * With this the client owns those pixels and draws its controls on the same
 * line as the title. The server still draws its own window controls on top,
 * because closing a window must not depend on the application, and it still
 * owns the drag: a press in that strip goes to the client, and a client that
 * did not want it calls win_move_begin to hand it back. That way the client
 * decides what is a control and the server decides what a drag is, which is
 * the only division that leaves neither guessing about the other. */
#define WS_FLAG_CLIENT_TITLE 4u

/* A sheet: a panel of this application's own, centred over the window it
 * belongs to, with no title bar, no controls, and no way to move or resize it.
 *
 * Dialogues used to be drawn into the window that raised them, which works
 * until the window's pixels are the document. Paint's canvas *is* its buffer -
 * there is no model behind it - so a save dialogue drawn over the picture
 * destroyed the part of the picture it covered, permanently, and then the
 * picture was saved with the hole in it. No amount of drawing order fixes that:
 * what is underneath was never stored anywhere.
 *
 * So a sheet is a real window. It has its own pixels, and the one it belongs to
 * is untouched beneath it. */
#define WS_FLAG_SHEET 8u

/* Not on screen for the moment, by the client's own choice.
 *
 * Not the same as being closed - the slot, the pixels and the event queue all
 * stay - and not the same as being behind something, because a hidden window
 * is not there to be clicked either. It is what a screen capture needs: the
 * one window that must not be in the picture is the window asking for it.
 *
 * The client sets and clears it. There is no dock to restore from, so the
 * server deliberately offers no way to un-hide a window it was not asked to:
 * a window nothing can bring back is a window that has been lost. */
#define WS_FLAG_HIDDEN 16u

/* Above the ordinary windows: the status bar, the dock, and the panels they
 * open.
 *
 * The desktop is the layer below everything and this is the layer above it, so
 * the order on screen is desktop, windows, overlays. An overlay stays there -
 * raising an ordinary window brings it to the front of the ordinary ones, not
 * in front of the bar, because a clock that disappears behind whatever was
 * clicked last is not a clock.
 *
 * And an overlay never takes the keyboard. It is chrome: clicking the dock to
 * start something should not stop the window you were typing in from receiving
 * what you type next. Focus goes to the frontmost window that is not one of
 * these. */
#define WS_FLAG_OVERLAY 32u

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

/* The wheel was turned over this window.
 *
 * `x` and `y` are the pointer, in the window's own coordinates, as they are in
 * every other pointer event - so a window with several scrolling views in it
 * can tell which one was under the wheel. `button` carries how far it turned,
 * in notches, positive downwards.
 *
 * Delivered to the window under the pointer rather than to the focused one,
 * because scrolling is about what you are looking at and not about what you
 * were last typing into. */
#define WIN_EVENT_SCROLL     8

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
    /* What was held when this happened - for a press and for a keystroke
     * alike. WIN_MOD_* below.
     *
     * It used to say a key event did not need them, on the grounds that shift
     * is already folded into the character. That is true of the letters and
     * false of everything else: an arrow has no shifted form, so shift+left
     * and left were the same event, and no text anywhere could be selected
     * with the keyboard. */
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
    /* Pixels per row of the client's buffer, which is not always the width.
     *
     * A window's buffer is allocated with room to spare so that a resize can
     * change what is shown without replacing anything: shared memory has no
     * realloc, so growing meant a new segment, a new mapping and a new
     * generation every time - once per frame of a drag, which is why the
     * server used to draw a wireframe and wait for the button to come up
     * instead of resizing as you moved.
     *
     * Zero means "the same as the width", which is what a client that has not
     * thought about it leaves behind. */
    volatile uint32_t stride;
    /* How wide this window's sidebar is, or zero. The server tints that column
     * across the title bar as well, because a sidebar that stops where the
     * chrome begins is a panel with a lid on it - the whole point of a full
     * height sidebar is that it is one surface from the top of the window. */
    volatile uint32_t sidebar;
    /* Bring this window to the front, asked for by somebody who is not it.
     *
     * A window raises itself by being clicked, which is the server's business
     * and needs nothing here. This is for the other case: a list of what is
     * running, with the one you pick coming forward. Set it and the server
     * raises the window and clears it; there is nothing to wait for. */
    volatile uint32_t raise_req;
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

    /* Bumped by a client that wants the window moved with the pointer - what a
     * press on its own title strip means when it was not on one of its
     * controls. The server compares against what it last saw, so a missed
     * poll delays a drag rather than losing it. */
    volatile uint32_t move_request;

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

/* What was here as well: `light`, `shadow` and `contrast`.
 *
 * They were the two edges of a bevel and how far apart to push them, which is
 * how a flat machine said "raised" without a second colour. Nothing has drawn
 * a bevel since the corners were rounded; depth comes from a shadow outside
 * the frame now. The three were still being written by the theme and by
 * Settings, and read by nobody. */
struct ws_theme {
    volatile uint32_t desktop;
    volatile uint32_t face;
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

    /* Whether a window's backdrop is frosted glass or a flat opaque panel.
     *
     * Blurring is by a wide margin the most expensive thing the compositor
     * does - about a thousand milliseconds per megapixel in the guest against
     * ten for a copy - so on a machine without acceleration it is the first
     * thing worth turning off. Off is the default for that reason: the glass
     * is what the interface wants to be, not what it has to be. */
    volatile uint32_t blur;         /* 1 frosted, 0 opaque                    */

    /* Whether a window casts a shadow.
     *
     * A shadow is a per-pixel blend over an area larger than the window it
     * falls behind, so on a machine with no acceleration it is the second
     * thing worth turning off after the glass. On by default, because a
     * desktop without them is a set of rectangles with no order to them. */
    volatile uint32_t shadows;
};

/* How the pointer and the keyboard behave. Here rather than in each driver
 * because these are the user's answers, not the hardware's, and every one of
 * them was a constant compiled into something before.
 *
 * The server owns the defaults and writes them at startup; anything that wants
 * to change one writes it and the driver reads it next time round. */
struct ws_input {
    volatile uint32_t natural_scroll;   /* 1: the content follows the wheel  */
    volatile uint32_t scroll_lines;     /* a notch is worth this many        */
    volatile uint32_t pointer_speed;    /* percent; 100 is one count, one px */
    /* What is deliberately not here: key repeat. A PS/2 keyboard repeats in
     * hardware and the rate is set with one command, but under emulation the
     * repeat comes from the host and that command is accepted and ignored - so
     * the setting would do nothing on the only machine this runs on. Doing it
     * in software instead would repeat twice on a real keyboard. Neither is a
     * setting, so there is not one. */
    /* How long the screen may sit untouched before it goes dark. Zero never
     * blanks it. Any key or movement brings it back. */
    volatile uint32_t blank_ms;
    /* Turn the screen off now, rather than after the idle time above. Set by
     * whoever offers a Sleep button; the compositor blanks and clears it. What
     * this system has instead of a suspend: there is no ACPI sleep state here,
     * and a button that claimed to suspend and only dimmed would be a lie. */
    volatile uint32_t blank_now;
};

/* What was here: four backdrop patterns - a grid, dots, a weave and a two-tone
 * dither - drawn when there was no wallpaper. They are what a machine with a
 * handful of colours did to make a shade it did not have, and this one has
 * millions of them and a photograph on the desktop by default. Removed rather
 * than left as a setting nobody chose, which is what they had become. */

/* Every bank after the first. Its own segment, under WS_BANK_KEY(b). */
struct ws_bank {
    struct ws_window windows[WS_BANK_WINDOWS];
};

/* Something a program wants to say that is not worth a window.
 *
 * A ring rather than a queue, and a sequence number rather than a count, so
 * that posting is one atomic increment and a write: a program saying something
 * must never wait for anything to read it, and a reader that is not running -
 * or has fallen behind - must not stop the ring from turning. A reader
 * remembers the last sequence it saw; anything above that is new, and anything
 * more than WS_NOTES_MAX behind has been overwritten and is gone, which is the
 * right thing to do with a stale notification.
 *
 * `seq` is written last and read first, so a reader never sees half a message:
 * a slot whose sequence is not the one expected is one being written. */
#define WS_NOTES_MAX  8
#define WS_NOTE_FROM  24
#define WS_NOTE_TEXT  144

struct ws_note {
    volatile uint32_t seq;              /* 0 until it has been written */
    volatile uint32_t at_ms;            /* uptime when it was posted   */
    char from[WS_NOTE_FROM];            /* the application's own name  */
    char text[WS_NOTE_TEXT];
};

struct ws_notes {
    volatile uint32_t next;             /* the sequence the next post takes */
    struct ws_note ring[WS_NOTES_MAX];
};

struct ws_shared {
    volatile uint32_t magic;
    volatile uint32_t server_pid;
    volatile uint32_t quit;         /* set by the server as it exits */
    /* How many banks exist, including the one below. Never less than 1, and
     * only ever raised - by a client that ran out of slots and made another. */
    volatile uint32_t banks;
    struct ws_theme theme;
    struct ws_input input;

    /* Where an ordinary window may go: the screen, less whatever the status
     * bar and the dock have taken.
     *
     * Written by whoever owns that furniture and read by everything that has
     * to place a window, so that a new one does not open underneath the clock.
     * A zero width means nobody has claimed anything and the whole screen is
     * available - which is what this was before there was a bar or a dock, and
     * what it still is if neither is running. */
    volatile int32_t work_x, work_y, work_w, work_h;
    struct ws_notes notes;
    struct ws_drag  drag;
    struct ws_window windows[WS_BANK_WINDOWS];
};

#endif /* _WPROTO_H */
