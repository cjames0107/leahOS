#ifndef _APP_H
#define _APP_H

#include <ui.h>
#include <widget.h>
#include <window.h>

/* An application, minus the sixty lines every application was writing itself.
 *
 * Every window program here opened with the same page of code: find the font,
 * create a window, turn on alpha, map its pixels, set a minimum size, point the
 * toolkit at the buffer, draw once, present, then loop on win_poll handling
 * resize by re-mapping and re-targeting, handling the menu before anything
 * else, and sleeping fifteen milliseconds at the bottom. Twelve copies of it,
 * and they had already drifted: some forgot win_set_alpha, some re-targeted the
 * toolkit after a resize and some did not, and the one that did not corrupted
 * its own drawing the first time the window changed size.
 *
 * That is the definition of something belonging in a library. What is left for
 * an application to write is what makes it that application: what to draw, what
 * to do with an event, and what to do on a timer.
 *
 * The smallest useful program is now:
 *
 *     static void draw(struct app* a) {
 *         wg_theme(); wg_glass_clear();
 *         wg_text(16, 16, "hello", wg_ink_colour());
 *     }
 *     int main(int argc, char** argv) {
 *         struct app a = { .title = "Hello", .width = 300, .height = 200,
 *                          .draw = draw };
 *         return app_run(&a, argc, argv);
 *     }
 */

struct app {
    /* --- what the author sets ------------------------------------------- */
    const char* title;
    unsigned    width, height;          /* the size to open at */
    unsigned    min_width, min_height;  /* 0 for no minimum */
    unsigned    sidebar;                /* width of a full-height sidebar, or 0
                                         * - the title bar is tinted to match */
    int         opaque;                 /* 1 to opt out of the glass */
    /* 1 to draw the title strip yourself. The server then stops treating a
     * press there as a drag, which is what lets controls sit on the same line
     * as the title - and makes it the window's job to call win_move_begin for
     * the parts of that line which are not controls. */
    int         client_title;

    /* An interface built from components. Set this and draw/event become
     * optional: app_run lays the tree out over the window, routes events into
     * it, and paints it - which for most windows is the whole of the loop.
     *
     * `draw` still runs first when both are set, so an application can paint a
     * background of its own beneath its components. */
    struct ui_view* root;

    /* Draw the whole window. Called when something changed, not on a clock;
     * the toolkit is already pointed at the buffer and the size is in a->w and
     * a->h, which are kept right across a resize. */
    void (*draw)(struct app* a);

    /* An event that is not the close box, a resize, or the menu - those are
     * handled before this is called. Return 1 to redraw. */
    int (*event)(struct app* a, const struct win_event* e);

    /* Before the components rather than after them, for the one thing that
     * has to be: an overlay that is modal. A file dialogue drawn over the
     * window is in front of every control in it, and a press that lands on a
     * button underneath must not reach that button. Return non-zero to say the
     * event was taken and stop it there.
     *
     * Only for that. Ordinary handling belongs in `event`, where a view has
     * already had its say. */
    int (*filter)(struct app* a, const struct win_event* e);

    /* And what that overlay draws, painted over the components and under the
     * pop-up menu - which is the order they sit in on screen. */
    void (*overlay)(struct app* a);

    /* Called every `tick_ms` milliseconds, or never when that is 0. Return 1
     * to redraw, which is what stops a monitor that changes twice a second
     * from redrawing sixty times a second. */
    unsigned tick_ms;
    int (*tick)(struct app* a);

    /* A right-click menu, if there is one. `pick` is the index chosen; return
     * 1 to redraw. */
    const char* const* menu;
    int                menu_count;
    int (*menu_pick)(struct app* a, int pick);

    void* user;                         /* the application's own state */

    /* --- sheets ----------------------------------------------------------
     *
     * A dialogue, as a window of its own centred on this one. It has its own
     * component tree, takes the keyboard and the pointer while it is up, and
     * leaves the window beneath it untouched - which is the whole point. A
     * dialogue drawn *into* its parent destroys whatever it covers, and for a
     * window whose pixels are the document that is the document.
     *
     * Opened with app_sheet, which returns the root to hang components on;
     * closed by the application calling app_sheet_close, usually from the
     * action on its buttons. `sheet_done` is told which answer came back.
     */
    struct ui_view* sheet;
    void (*sheet_done)(struct app* a, int result);

    /* --- what app_run fills in ------------------------------------------ */
    int        id;                      /* the window */
    uint32_t*  px;                      /* its pixels */
    unsigned   w, h;                    /* its current size */
    int        quit;                    /* set by app_quit */
    /* Whether a component already took the event `event` is being called
     * with. The handler is called either way - an application may well want to
     * know about a click its list has just dealt with - but anything that acts
     * on a bare press has to look at this, or a menu item chosen over a
     * document also lands in the document. */
    int        handled;
    int        status;                  /* what main should return */

    /* The sheet's own window, filled in by app_sheet. */
    int        sheet_id;
    uint32_t*  sheet_px;
    unsigned   sheet_w, sheet_h;
};

/* Open the window and run until it closes. argv[1] and argv[2], when present,
 * are the position to open at - which is the convention the desktop already
 * uses to launch a bundle, so an application gets it by doing nothing. */
int app_run(struct app* a, int argc, char** argv);

/* Rebuild the component tree after its shape has changed - a list gaining
 * rows, a pane being swapped. Layout is re-run on the next pass. */
void app_relayout(struct app* a);

/* Open a sheet of `w` by `h`, centred on the window, and return the root to
 * build it out of. Only one at a time: opening a second closes the first,
 * because two modal panels over one window is not a thing that can be answered.
 *
 * The tree comes from the same pool as everything else, so an application that
 * opens sheets repeatedly should keep the root and reuse it rather than
 * building a new one each time. */
struct ui_view* app_sheet(struct app* a, unsigned w, unsigned h);

/* Take the sheet down and tell sheet_done what the answer was. */
void app_sheet_close(struct app* a, int result);

int app_sheet_open(const struct app* a);

/* A ready-made saving sheet: a name to type, Cancel and Save. sheet_done is
 * called with 1 when Save was chosen, and app_sheet_path then holds the full
 * path. Provided rather than left to each application, because it is the same
 * question every time. */
struct ui_view* app_sheet_save(struct app* a, const char* dir,
                               const char* suggested);

/* The same panel with its own words. `title` is the line above the field and
 * `action` is what the button says; either may be 0 for "Save as" and "Save".
 *
 * Asking for a name is not always saving. Files uses this to rename, and a
 * panel that says "Save" over a file somebody meant to rename is a panel that
 * has to be stopped and thought about. */
struct ui_view* app_sheet_name(struct app* a, const char* dir,
                               const char* suggested, const char* title,
                               const char* action);
const char* app_sheet_path(const struct app* a);

/* A sheet for choosing a file: the directory listed, directories entered by
 * choosing them, a file answered with. app_sheet_path holds it. */
struct ui_view* app_sheet_file(struct app* a, const char* dir);

/* A sheet for choosing which application opens a document.
 *
 * The installed applications are walked rather than listed here: "which
 * programs can open a file" is not something the filesystem records, and a
 * panel that named three of them would be wrong the moment an eleventh was
 * installed and would be the last place anyone thought to look.
 *
 * app_sheet_path holds the chosen program's command. app_sheet_always says
 * whether the person asked for the choice to stick, which the caller
 * remembers - the framework has no business knowing what a document is. */
struct ui_view* app_sheet_open_with(struct app* a, const char* document);
int app_sheet_always(const struct app* a);

/* A sheet for choosing a date. app_sheet_path holds it as YYYY-MM-DD, which is
 * the same way every other sheet returns its answer. */
struct ui_view* app_sheet_date(struct app* a, int year, int month, int day);

/* Ask the loop to stop. The window is destroyed and app_run returns. */
void app_quit(struct app* a, int status);

/* Force a redraw from outside an event - after finishing a slow load, say.
 * Inside a handler, returning 1 is simpler and does the same thing. */
void app_redraw(struct app* a);

#endif
