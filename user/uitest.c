/* uitest - one window containing every drawing and input element the desktop
 * has, so that a change to the window server can be checked by looking at it.
 *
 * There is no widget toolkit: a client owns a rectangle of pixels and draws it
 * itself, so "a button" means a bevelled rectangle that this program repaints
 * pressed when the pointer goes down inside it. That is the whole of the
 * abstraction, and showing it plainly is more useful than hiding it.
 *
 * What it covers:
 *   - raised and sunken bevels, the idiom the chrome is built from
 *   - push buttons that depress under the pointer and latch a counter
 *   - a checkbox and a set of radio buttons, to show click routing by region
 *   - the console font, drawn by the client rather than by the server
 *   - a colour bar, to prove the pixel format end to end
 *   - a live readout of the last event, which is the input path made visible
 *   - a size readout that follows a resize, and a redraw that follows it
 */

#include <display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <window.h>

#define FACE    0xC0C0C0
#define LIGHT   0xFFFFFF
#define SHADOW  0x606060
#define INK     0x000000
#define ACCENT  0x000080
#define PAPER   0xFFFFFF

static uint32_t* g_px;
static unsigned  g_w, g_h;
static unsigned char g_font[256 * 16];

/* --- primitives ---------------------------------------------------------- */

static void plot(int x, int y, uint32_t colour)
{
    if (x < 0 || y < 0 || (unsigned)x >= g_w || (unsigned)y >= g_h)
        return;
    g_px[(unsigned)y * g_w + (unsigned)x] = colour;
}

static void fill(int x, int y, int w, int h, uint32_t colour)
{
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            plot(x + col, y + row, colour);
}

/* Raised, or inverted for sunken: light on the top and left, shadow on the
 * bottom and right. Two shades either side of the face colour is all it takes
 * for the eye to read depth. */
static void bevel(int x, int y, int w, int h, int raised)
{
    const uint32_t tl = raised ? LIGHT : SHADOW;
    const uint32_t br = raised ? SHADOW : LIGHT;
    for (int i = 0; i < w; ++i) {
        plot(x + i, y, tl);
        plot(x + i, y + h - 1, br);
    }
    for (int i = 0; i < h; ++i) {
        plot(x, y + i, tl);
        plot(x + w - 1, y + i, br);
    }
}

static void text(int x, int y, const char* s, uint32_t colour)
{
    for (unsigned i = 0; s[i] != '\0'; ++i) {
        const unsigned char* glyph = &g_font[(unsigned char)s[i] * 16];
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 8; ++col)
                if (glyph[row] & (0x80 >> col))
                    plot(x + (int)i * 8 + col, y + row, colour);
    }
}

/* --- the elements -------------------------------------------------------- */

struct box { int x, y, w, h; };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

static struct box g_button[2];
static struct box g_check;
static struct box g_radio[3];

static int g_pressed = -1;      /* which button the pointer is holding down */
static int g_clicks;
static int g_checked = 1;
static int g_choice;
static char g_last[64] = "waiting for an event";

static void layout(void)
{
    g_button[0].x = 16;  g_button[0].y = 44; g_button[0].w = 96; g_button[0].h = 26;
    g_button[1].x = 124; g_button[1].y = 44; g_button[1].w = 96; g_button[1].h = 26;
    g_check.x = 16; g_check.y = 88; g_check.w = 14; g_check.h = 14;
    for (int i = 0; i < 3; ++i) {
        g_radio[i].x = 16 + i * 84;
        g_radio[i].y = 116;
        g_radio[i].w = 14;
        g_radio[i].h = 14;
    }
}

static void draw(void)
{
    fill(0, 0, (int)g_w, (int)g_h, FACE);

    /* A sunken well around the whole content, so the client's area is visibly
     * distinct from the frame the server drew around it. */
    bevel(0, 0, (int)g_w, (int)g_h, 0);
    text(12, 12, "leahOS interface elements", INK);

    /* Push buttons. The pressed one is drawn sunken and shifted a pixel, which
     * is the entire animation budget of this era and reads perfectly well. */
    static const char* labels[2] = { "Click me", "And me" };
    for (int i = 0; i < 2; ++i) {
        const int down = (g_pressed == i);
        fill(g_button[i].x, g_button[i].y, g_button[i].w, g_button[i].h, FACE);
        bevel(g_button[i].x, g_button[i].y, g_button[i].w, g_button[i].h, !down);
        text(g_button[i].x + 10 + down, g_button[i].y + 5 + down, labels[i], INK);
    }

    /* Checkbox: sunken well with a tick when set. */
    fill(g_check.x, g_check.y, g_check.w, g_check.h, PAPER);
    bevel(g_check.x, g_check.y, g_check.w, g_check.h, 0);
    if (g_checked) {
        for (int i = 0; i < 5; ++i)
            plot(g_check.x + 3 + i, g_check.y + 6 + i, ACCENT);
        for (int i = 0; i < 5; ++i)
            plot(g_check.x + 7 + i, g_check.y + 10 - i, ACCENT);
    }
    text(g_check.x + 22, g_check.y - 1, g_checked ? "checked" : "unchecked", INK);

    /* Radio buttons: only one at a time, which is the point of them. */
    for (int i = 0; i < 3; ++i) {
        fill(g_radio[i].x, g_radio[i].y, g_radio[i].w, g_radio[i].h, PAPER);
        bevel(g_radio[i].x, g_radio[i].y, g_radio[i].w, g_radio[i].h, 0);
        if (g_choice == i)
            fill(g_radio[i].x + 4, g_radio[i].y + 4, 6, 6, ACCENT);
        char label[8] = { 'o', 'n', 'e', 0 };
        if (i == 1) { label[0] = 't'; label[1] = 'w'; label[2] = 'o'; }
        if (i == 2) { label[0] = 't'; label[1] = 'e'; label[2] = 'n'; }
        text(g_radio[i].x + 20, g_radio[i].y - 1, label, INK);
    }

    /* A colour bar: if the pixel format is wrong this is the first thing to
     * look wrong, and it is wrong in an obvious way. */
    static const uint32_t swatch[8] = {
        0x000000, 0x800000, 0x008000, 0x808000,
        0x000080, 0x800080, 0x008080, 0xFFFFFF,
    };
    for (int i = 0; i < 8; ++i)
        fill(16 + i * 24, 148, 24, 18, swatch[i]);
    bevel(16, 148, 8 * 24, 18, 0);

    /* Readouts: the last event, the click count, and the current size. The
     * size line is what a resize has to change. */
    char line[96];
    snprintf(line, sizeof(line), "clicks %d   size %ux%u", g_clicks, g_w, g_h);
    text(16, 176, line, INK);

    fill(12, 198, (int)g_w - 24, 20, PAPER);
    bevel(12, 198, (int)g_w - 24, 20, 0);
    text(16, 200, g_last, INK);

    text(16, 226, "keys go to the focused window; ctrl+q closes", INK);
}

static void note(const char* what, int x, int y)
{
    char line[64];
    snprintf(line, sizeof(line), "%s at %d,%d", what, x, y);
    unsigned i = 0;
    while (line[i] != '\0' && i < sizeof(g_last) - 1) {
        g_last[i] = line[i];
        ++i;
    }
    g_last[i] = '\0';
}

int main(int argc, char** argv)
{
    const int x = argc > 1 ? atoi_simple(argv[1]) : 120;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 120;

    if (fb_font(g_font) != 0) {
        printf("uitest: cannot read the console font\n");
        return 1;
    }

    g_w = 300;
    g_h = 250;
    const int id = win_create(x, y, g_w, g_h, "Elements");
    if (id < 0) {
        printf("uitest: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 260, 250);

    layout();
    draw();
    win_present(id);

    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)event.x;
                g_h = (unsigned)event.y;
                g_px = win_map(id);
                if (g_px == 0)
                    return 1;
                note("resized to", event.x, event.y);
                layout();
                draw();
                win_present(id);
                continue;
            }
            if (event.type == WIN_EVENT_CLOSE) {
                win_destroy(id);
                return 0;
            }
            if (event.type == WIN_EVENT_MOUSE_DOWN) {
                note("press", event.x, event.y);
                g_pressed = -1;
                for (int i = 0; i < 2; ++i)
                    if (inside(&g_button[i], event.x, event.y))
                        g_pressed = i;
                if (inside(&g_check, event.x, event.y))
                    g_checked = !g_checked;
                for (int i = 0; i < 3; ++i)
                    if (inside(&g_radio[i], event.x, event.y))
                        g_choice = i;
                draw();
                win_present(id);
            } else if (event.type == WIN_EVENT_MOUSE_UP) {
                note("release", event.x, event.y);
                /* A click is a press and a release on the same button, which
                 * is why the press is only latched here. */
                if (g_pressed >= 0)
                    ++g_clicks;
                g_pressed = -1;
                draw();
                win_present(id);
            } else if (event.type == WIN_EVENT_MOUSE_MOVE) {
                note("drag", event.x, event.y);
                draw();
                win_present(id);
            } else if (event.type == WIN_EVENT_KEY) {
                char line[48];
                snprintf(line, sizeof(line), "key '%c'", (char)event.key);
                unsigned i = 0;
                while (line[i] != '\0' && i < sizeof(g_last) - 1) {
                    g_last[i] = line[i];
                    ++i;
                }
                g_last[i] = '\0';
                draw();
                win_present(id);
            }
        }
        msleep(15);
    }
}
