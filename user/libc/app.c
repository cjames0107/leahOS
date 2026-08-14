/* app_run - the loop every window program was writing for itself.
 *
 * See <app.h> for why this exists. The rules it enforces, which are the ones
 * the hand-written copies kept getting wrong:
 *
 *  - After a resize the pixel buffer is a different buffer. It has to be
 *    re-mapped and the toolkit re-pointed at it, in that order, before
 *    anything draws. An application that missed either drew into the old
 *    mapping and left the new one full of whatever was there.
 *
 *  - The menu eats events while it is open, except a resize, which nothing may
 *    swallow.
 *
 *  - Drawing happens once per pass through the pending events, not once per
 *    event. A drag delivers a stream of motion, and redrawing per motion event
 *    is how a window server ends up composing the same frame five times.
 */

#include <app.h>
#include <dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void app_quit(struct app* a, int status)
{
    a->quit = 1;
    a->status = status;
}

static void paint(struct app* a);

void app_redraw(struct app* a)
{
    if (a->px != 0)
        paint(a);
}

/* Take the new size and make every view of it agree. */
static int resized(struct app* a, int w, int h)
{
    a->w = (unsigned)w;
    a->h = (unsigned)h;
    a->px = win_map(a->id);
    if (a->px == 0)
        return -1;
    wg_target(a->px, a->w, a->h);
    /* The components are laid out against the window, so a new window size is
     * a new layout - and it happens here rather than at the next draw, because
     * an event may arrive first and would otherwise be hit-tested against the
     * old frames. */
    app_relayout(a);
    return 0;
}

/* Paint: the application's own background if it has one, then its components
 * over the top. */
static void paint(struct app* a)
{
    if (a->draw != 0)
        a->draw(a);
    else {
        wg_theme();
        wg_glass_clear();
    }
    if (a->root != 0)
        ui_draw(a->root);
    menu_draw();
    win_present(a->id);
}

void app_relayout(struct app* a)
{
    if (a == 0 || a->root == 0)
        return;
    const struct ui_rect all = { 0, 0, (int)a->w, (int)a->h };
    ui_layout(a->root, all);
}

int app_run(struct app* a, int argc, char** argv)
{
    /* One or the other has to exist, or there is nothing to show. */
    if (a == 0 || (a->draw == 0 && a->root == 0))
        return 1;
    if (wg_font() != 0)
        return 1;

    const int x = argc > 1 ? atoi_simple(argv[1]) : 160;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 120;
    if (a->width == 0)  a->width = 640;
    if (a->height == 0) a->height = 420;

    a->id = win_create(x, y, a->width, a->height,
                       a->title != 0 ? a->title : "Window");
    if (a->id < 0) {
        printf("%s: no window server\n", a->title != 0 ? a->title : "app");
        return 1;
    }
    /* Alpha unless the application asks to be opaque, because the glass
     * reaching into a window is the default look and forgetting to ask for it
     * was the single most common way a new window came out wrong. */
    if (!a->opaque)
        win_set_alpha(a->id);
    if (a->sidebar > 0)
        win_set_sidebar(a->id, a->sidebar);
    if (a->min_width > 0 || a->min_height > 0)
        win_set_min_size(a->id, a->min_width, a->min_height);

    a->w = a->width;
    a->h = a->height;
    a->px = win_map(a->id);
    if (a->px == 0)
        return 1;
    wg_target(a->px, a->w, a->h);

    app_relayout(a);
    paint(a);

    /* The tick is counted in fifteen-millisecond passes rather than measured,
     * which is what the hand-written loops did and is accurate enough for
     * something whose purpose is "about twice a second". */
    const unsigned every = a->tick_ms > 0 ? (a->tick_ms / 15 + 1) : 0;
    unsigned since = 0;

    while (!a->quit) {
        int dirty = 0;
        struct win_event e;
        while (win_poll(a->id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) {
                win_destroy(a->id);
                return a->status;
            }
            if (e.type == WIN_EVENT_RESIZE) {
                if (resized(a, e.x, e.y) != 0)
                    return 1;
                dirty = 1;
                continue;
            }
            if (menu_active()) {
                const int pick = menu_event(&e);
                if (pick >= 0 && a->menu_pick != 0)
                    dirty |= a->menu_pick(a, pick);
                dirty = 1;              /* the menu itself has to be redrawn */
                continue;
            }
            /* Components first: they are the interface, and an application's
             * own handler is for what is left over. A view that took the event
             * says so by asking for a redraw, and the handler still sees it -
             * an application may want to know about a click its list already
             * dealt with. */
            if (a->root != 0)
                dirty |= ui_event(a->root, &e);
            if (a->event != 0)
                dirty |= a->event(a, &e);
            if (e.type == WIN_EVENT_MOUSE_DOWN && e.button == 2 &&
                a->menu != 0 && a->menu_count > 0 && !menu_active()) {
                menu_open(e.x, e.y, a->menu, a->menu_count);
                dirty = 1;
            }
        }

        if (every > 0 && ++since >= every) {
            since = 0;
            if (a->tick != 0)
                dirty |= a->tick(a);
        }

        if (dirty && !a->quit)
            paint(a);
        msleep(15);
    }

    win_destroy(a->id);
    return a->status;
}
