#ifndef _WINDOW_H
#define _WINDOW_H

#include <stdint.h>

/* Talking to the window server.
 *
 * A window is a rectangle of 32-bit pixels the server maps into this process.
 * Drawing means writing to that memory and then calling win_present; the client
 * never touches the screen itself and cannot see any other window's pixels. */

enum {
    WIN_EVENT_NONE = 0,
    WIN_EVENT_MOUSE_DOWN,
    WIN_EVENT_MOUSE_UP,
    WIN_EVENT_MOUSE_MOVE,
    WIN_EVENT_KEY,
    WIN_EVENT_CLOSE,        /* the close box was clicked */
};

struct win_event {
    uint32_t type;
    uint32_t window;
    int32_t  x;             /* relative to the content area */
    int32_t  y;
    uint32_t button;
    uint32_t key;
};

/* Open a window of `width` x `height` content pixels. Returns its id, or -1. */
int win_create(int x, int y, unsigned width, unsigned height, const char* title);

/* The window's pixel buffer, `width * height` packed as 0x00RRGGBB. */
uint32_t* win_map(int id);

/* Show what has been drawn. */
void win_present(int id);

/* Take the next event, or return 0 when there is none. */
int win_poll(int id, struct win_event* out);

void win_destroy(int id);

#endif /* _WINDOW_H */
