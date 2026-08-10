#ifndef _WINDOW_H
#define _WINDOW_H

#include <stdint.h>
#include <wproto.h>

/* Talking to the window server.
 *
 * A window is a rectangle of 32-bit pixels in a shared memory segment that both
 * this process and the server map. Drawing means writing to that memory and
 * calling win_present; the client never touches the screen itself, and the
 * pixels stay owned by this process so no other user can map them.
 *
 * struct win_event and the WIN_EVENT_* constants come from <wproto.h>, which is
 * the protocol both sides share. */

/* Open a window of `width` x `height` content pixels. Returns its id (which is
 * its slot in the server's table), or -1 when there is no server or no room. */
int win_create(int x, int y, unsigned width, unsigned height, const char* title);

/* The window's pixel buffer, `width * height` packed as 0x00RRGGBB.
 *
 * Not stable across a resize: WIN_EVENT_RESIZE means this buffer has been
 * replaced, so call it again rather than holding the old pointer. */
uint32_t* win_map(int id);

/* The window's current content size. */
void win_size(int id, unsigned* width, unsigned* height);

/* Refuse to be resized below this. Defaults to 64x32. */
void win_set_min_size(int id, unsigned width, unsigned height);

/* Make this the desktop: no chrome, always behind every other window, and not
 * draggable. Call before drawing; see WS_FLAG_DESKTOP in <wproto.h>. */
void win_set_desktop(int id);

/* Say that this window's pixels carry alpha, so the server blends them onto
 * the blurred backdrop instead of copying over it. A window that says this
 * must fill its alpha byte everywhere - zero means invisible, not opaque. */
void win_set_alpha(int id);

/* How wide this window's sidebar is. The server carries the same tint up
 * across the title bar, so the sidebar is one surface from the very top of the
 * window rather than a panel starting under the chrome. */
void win_set_sidebar(int id, unsigned width);

/* Show what has been drawn. */
void win_present(int id);

/* Take the next event, or return 0 when there is none. */
int win_poll(int id, struct win_event* out);

void win_destroy(int id);

/* --- dragging things between windows --------------------------------------
 *
 * The server carries the ghost, because it is the only thing that can draw
 * over every window at once. A source calls win_drag_begin and then forgets
 * about it; the drop lands as a WIN_EVENT_DROP on whichever window the cursor
 * was over, which may well belong to another process. */

/* Start dragging `path`. `icon` is WS_DRAG_FILE / FOLDER / APP, `grab_x/y` is
 * where inside the ghost the cursor is holding it, and `home_x/y` is where it
 * came from on screen - the place it slides back to if nobody takes it. */
void win_drag_begin(const char* path, const char* label, unsigned icon,
                    int grab_x, int grab_y, int home_x, int home_y);

/* What is being dragged right now, or "" when nothing is. A source uses this
 * to stop drawing the item it is holding. */
const char* win_drag_path(void);
int         win_dragging(void);

/* What was dropped. Unlike win_drag_path this answers after the drag has
 * ended, which is exactly when a receiver asks: the server leaves the record
 * intact until the next drag starts. */
const char* win_drop_path(void);

/* Answer a WIN_EVENT_DROP. Accepting takes a screen position for the ghost to
 * settle into; rejecting sends it back where it came from. One of the two must
 * be called, or the ghost stays on screen. */
void win_drop_accept(int screen_x, int screen_y);
void win_drop_reject(void);

/* The window's position on screen, so a client can turn its own coordinates
 * into the screen ones the drag record wants. */
void win_origin(int id, int* x, int* y);

/* Whether a server is running, so a client can say so rather than just fail. */
int win_server_running(void);

#endif /* _WINDOW_H */
