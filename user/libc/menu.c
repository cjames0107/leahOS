/* Pop-up menus.
 *
 * A menu is drawn by the application over its own window rather than being a
 * window of its own: the window server has no notion of a popup, and a menu is
 * on screen for the length of one gesture. That is the one thing here that is
 * still true of the file dialogue this used to share a file with - and the
 * only thing, which is why they no longer do.
 */

#include <menu.h>
#include <string.h>
#include <widget.h>

/* --- context menus --------------------------------------------------------
 *
 * Kept apart from the dialogue state on purpose: choosing from a menu is very
 * often what raises a dialogue, so the two have to be able to exist at once.
 */
#define MENU_MAX  10
#define MENU_ROW  17

static int  g_menu_on;
static int  g_mx, g_my, g_mw;
static const char* const* g_menu_items;
static int  g_menu_n;

void menu_open(int x, int y, const char* const* items, int count)
{
    g_menu_on = 1;
    g_mx = x;
    g_my = y;
    g_menu_items = items;
    g_menu_n = count > MENU_MAX ? MENU_MAX : count;
    g_mw = 0;
    for (int i = 0; i < g_menu_n; ++i) {
        const int w = (int)strlen(items[i]) * WG_GLYPH_W + 24;
        if (w > g_mw) g_mw = w;
    }
    if (g_mw < 100) g_mw = 100;
}

int menu_active(void) { return g_menu_on; }

int menu_event(const struct win_event* e)
{
    if (!g_menu_on)
        return -1;
    if (e->type == WIN_EVENT_KEY) {
        g_menu_on = 0;
        return -2;                      /* any key dismisses it */
    }
    if (e->type != WIN_EVENT_MOUSE_DOWN)
        return -1;

    const int h = g_menu_n * MENU_ROW + 4;
    if (e->x < g_mx || e->x >= g_mx + g_mw || e->y < g_my || e->y >= g_my + h) {
        /* A click outside is a dismissal, and is *not* passed on to the
         * application: the first click after a menu closes it and nothing
         * else, which is what every menu everywhere does. */
        g_menu_on = 0;
        return -2;
    }
    const int i = (e->y - g_my - 2) / MENU_ROW;
    g_menu_on = 0;
    if (i < 0 || i >= g_menu_n || g_menu_items[i][0] == '-')
        return -2;
    return i;
}

void menu_draw(void)
{
    if (!g_menu_on)
        return;
    const int h = g_menu_n * MENU_ROW + 4;
    wg_fill(g_mx + 3, g_my + 3, g_mw, h, WG_SHADOW);
    wg_fill(g_mx, g_my, g_mw, h, WG_FACE);
    wg_outline(g_mx, g_my, g_mw, h, 1);
    for (int i = 0; i < g_menu_n; ++i) {
        const int y = g_my + 2 + i * MENU_ROW;
        if (g_menu_items[i][0] == '-') {
            wg_fill(g_mx + 4, y + MENU_ROW / 2, g_mw - 8, 1, WG_SHADOW);
            continue;
        }
        wg_text_clipped(g_mx + 10, y, g_menu_items[i], WG_INK, g_mw - 16);
    }
}
