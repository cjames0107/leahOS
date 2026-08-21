#ifndef _MENU_H
#define _MENU_H

#include <window.h>

/* Pop-up menus.
 *
 * A menu is something the application draws over its own window: the window
 * server has no notion of a popup, and a menu is on screen for the length of
 * one gesture, so a window of its own would be a window created and destroyed
 * inside a mouse click.
 *
 * This used to live with the file dialogue, which was the same overlay idea
 * for a much worse reason - a dialogue that draws into the window behind it
 * destroys what it covers, which is why dialogues are real windows now. The
 * menu kept the arrangement because for a menu it is the right one.
 */

/* Show a menu at the point it was asked for. `items` is an array of labels; a
 * label of "-" draws a separator and cannot be chosen. */
void menu_open(int x, int y, const char* const* items, int count);
int  menu_active(void);

/* Feed an event. Returns the chosen index, -1 while pending, and -2 when it was
 * dismissed without a choice. */
int  menu_event(const struct win_event* event);
void menu_draw(void);

#endif /* _MENU_H */
