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

    /* --- what app_run fills in ------------------------------------------ */
    int        id;                      /* the window */
    uint32_t*  px;                      /* its pixels */
    unsigned   w, h;                    /* its current size */
    int        quit;                    /* set by app_quit */
    int        status;                  /* what main should return */
};

/* Open the window and run until it closes. argv[1] and argv[2], when present,
 * are the position to open at - which is the convention the desktop already
 * uses to launch a bundle, so an application gets it by doing nothing. */
int app_run(struct app* a, int argc, char** argv);

/* Rebuild the component tree after its shape has changed - a list gaining
 * rows, a pane being swapped. Layout is re-run on the next pass. */
void app_relayout(struct app* a);

/* Ask the loop to stop. The window is destroyed and app_run returns. */
void app_quit(struct app* a, int status);

/* Force a redraw from outside an event - after finishing a slow load, say.
 * Inside a handler, returning 1 is simpler and does the same thing. */
void app_redraw(struct app* a);

#endif
