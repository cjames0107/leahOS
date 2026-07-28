#ifndef _DIALOG_H
#define _DIALOG_H

#include <wproto.h>

/* Modal dialogues, drawn inside the window that raised them.
 *
 * There are no child windows: the server knows about top-level windows and
 * nothing else. A dialogue is therefore an overlay an application paints over
 * its own content, and modality is a rule the application keeps rather than one
 * the system enforces - while one is up, events go to the dialogue first and
 * the application does not act on them.
 *
 * It is a state machine rather than a function that runs its own loop, because
 * a client also has to keep answering the server (a resize, a close) while a
 * dialogue is open. The shape is:
 *
 *     if (dlg_active()) {
 *         switch (dlg_event(&event)) {
 *         case DLG_ACCEPT: ... dlg_path() ...; break;
 *         case DLG_CANCEL: break;
 *         }
 *     } else {
 *         ... the application's own handling ...
 *     }
 *     ... draw the application ...
 *     dlg_draw(width, height);
 */

#define DLG_PENDING 0
#define DLG_ACCEPT  1
#define DLG_CANCEL  2

/* Ask where to save. `suggested` is the initial name, `where` the directory to
 * start in. */
void dlg_save(const char* where, const char* suggested);

/* Ask which application should open `path`. The choices are the programs in
 * /BIN that are plausible openers, plus running it directly when it is one. */
void dlg_open_with(const char* path);

int  dlg_active(void);

/* Feed one window event. Returns DLG_PENDING, DLG_ACCEPT or DLG_CANCEL; the
 * dialogue closes itself on the latter two. */
int  dlg_event(const struct win_event* event);

/* Draw the overlay, centred in a window of this size. Call after the
 * application has drawn itself. */
void dlg_draw(int window_w, int window_h);

/* What was chosen. After a save, the full path to write; after an open-with,
 * the program to run. */
const char* dlg_path(void);

/* After an open-with, the file that was being opened. */
const char* dlg_subject(void);

#endif /* _DIALOG_H */

/* --- context menus --------------------------------------------------------
 *
 * The same overlay idea as a dialogue, and for the same reason: there are no
 * popup windows, so a menu is something the application draws over itself. It
 * is separate from the dialogue state so that a menu can raise a dialogue.
 */

/* Show a menu at the point it was asked for. `items` is a NUL-terminated array
 * of labels; a label of "-" draws a separator and cannot be chosen. */
void menu_open(int x, int y, const char* const* items, int count);
int  menu_active(void);

/* Feed an event. Returns the chosen index, -1 while pending, and -2 when it was
 * dismissed without a choice. */
int  menu_event(const struct win_event* event);
void menu_draw(void);

/* --- the "always" checkbox ------------------------------------------------ */

/* Whether the open-with dialogue's "always open this kind with it" box was
 * ticked when the choice was accepted. */
int  dlg_always(void);
