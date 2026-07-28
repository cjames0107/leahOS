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

/* Show what has been drawn. */
void win_present(int id);

/* Take the next event, or return 0 when there is none. */
int win_poll(int id, struct win_event* out);

void win_destroy(int id);

/* Whether a server is running, so a client can say so rather than just fail. */
int win_server_running(void);

#endif /* _WINDOW_H */
