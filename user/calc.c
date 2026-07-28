/* calc - a calculator.
 *
 * Integer arithmetic, because this system has no floating point in userland:
 * the kernel builds with SSE off and nothing sets up an FPU context per task.
 * Division therefore truncates, and says so rather than pretending otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define COLS 4
#define ROWS 5
#define PAD  4

static uint32_t* g_px;
static unsigned  g_w = 240, g_h = 300;

static long g_shown;            /* what is on the display */
static long g_stored;           /* the left-hand operand */
static char g_op;               /* pending operation, 0 for none */
static int  g_fresh = 1;        /* the next digit starts a new number */
static char g_note[48] = "";

static const char* kKeys[ROWS][COLS] = {
    { "7", "8", "9", "/" },
    { "4", "5", "6", "*" },
    { "1", "2", "3", "-" },
    { "0", "+/-", "=", "+" },
    { "C", "<-", "",  ""  },
};

static void key_box(int r, int c, int* x, int* y, int* w, int* h)
{
    const int top = 60;
    const int cw = ((int)g_w - PAD * (COLS + 1)) / COLS;
    const int chh = ((int)g_h - top - PAD * (ROWS + 1)) / ROWS;
    *x = PAD + c * (cw + PAD);
    *y = top + PAD + r * (chh + PAD);
    *w = cw;
    *h = chh;
}

static void apply(void)
{
    if (g_op == 0)
        return;
    g_note[0] = '\0';
    switch (g_op) {
    case '+': g_stored += g_shown; break;
    case '-': g_stored -= g_shown; break;
    case '*': g_stored *= g_shown; break;
    case '/':
        if (g_shown == 0) {
            snprintf(g_note, sizeof(g_note), "cannot divide by zero");
            g_stored = 0;
        } else {
            if (g_stored % g_shown != 0)
                snprintf(g_note, sizeof(g_note), "truncated: integers only");
            g_stored /= g_shown;
        }
        break;
    }
    g_shown = g_stored;
}

static void press(const char* k)
{
    if (k[0] == '\0')
        return;
    if (k[0] >= '0' && k[0] <= '9' && k[1] == '\0') {
        if (g_fresh) { g_shown = 0; g_fresh = 0; }
        if (g_shown < 100000000L && g_shown > -100000000L)
            g_shown = g_shown * 10 + (k[0] - '0');
        return;
    }
    if (strcmp(k, "C") == 0) {
        g_shown = g_stored = 0; g_op = 0; g_fresh = 1; g_note[0] = '\0';
        return;
    }
    if (strcmp(k, "<-") == 0) { g_shown /= 10; return; }
    if (strcmp(k, "+/-") == 0) { g_shown = -g_shown; return; }
    if (strcmp(k, "=") == 0) {
        apply();
        g_op = 0; g_fresh = 1;
        return;
    }
    /* An operator: finish any pending one first, so 2+3+4 works left to right. */
    apply();
    g_stored = g_shown;
    g_op = k[0];
    g_fresh = 1;
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);

    wg_fill(PAD, PAD, (int)g_w - PAD * 2, 34, WG_PAPER);
    wg_bevel(PAD, PAD, (int)g_w - PAD * 2, 34, 0);
    char line[32];
    snprintf(line, sizeof(line), "%ld", g_shown);
    const int tw = (int)strlen(line) * WG_GLYPH_W;
    wg_text((int)g_w - PAD - 6 - tw, PAD + 9, line, WG_INK);
    if (g_op != 0) {
        char o[2] = { g_op, 0 };
        wg_text(PAD + 6, PAD + 9, o, WG_ACCENT);
    }
    wg_text_clipped(PAD + 2, 44, g_note, WG_DIM, (int)g_w - PAD * 2);

    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) {
            if (kKeys[r][c][0] == '\0')
                continue;
            int x, y, w, h;
            key_box(r, c, &x, &y, &w, &h);
            wg_button(x, y, w, h, kKeys[r][c], 0);
        }
}

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 320;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 120;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Calculator");
    if (id < 0) {
        printf("calc: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 200, 260);
    wg_target(g_px, g_w, g_h);
    draw();
    win_present(id);

    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }
            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                for (int r = 0; r < ROWS; ++r)
                    for (int c = 0; c < COLS; ++c) {
                        int x, y, w, h;
                        key_box(r, c, &x, &y, &w, &h);
                        if (e.x >= x && e.y >= y && e.x < x + w && e.y < y + h)
                            press(kKeys[r][c]);
                    }
            } else if (e.type == WIN_EVENT_KEY) {
                const char k = (char)e.key;
                char s[2] = { k, 0 };
                if ((k >= '0' && k <= '9') || k == '+' || k == '-' ||
                    k == '*' || k == '/')
                    press(s);
                else if (k == '\n' || k == '\r' || k == '=')
                    press("=");
                else if (k == 'c')
                    press("C");
                else if (k == '\b')
                    press("<-");
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
