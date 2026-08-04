/* calc - a calculator.
 *
 * Floating point, now that there is some: doubles throughout, and the
 * functions come from libc's maths library rather than from a table of
 * approximations kept here. Division no longer truncates and no longer has to
 * apologise for it.
 *
 * The angle mode is on the keypad rather than buried somewhere, because "sin
 * 30 is 0.5" and "sin 30 is -0.988" are both right and the difference between
 * them is the single most common reason to distrust a calculator.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define COLS 5
#define ROWS 7
#define PAD  4

static uint32_t* g_px;
static unsigned  g_w = 300, g_h = 390;

static double g_shown;          /* what is on the display */
static double g_stored;         /* the left-hand operand */
static char   g_op;             /* pending operation, 0 for none */
static int    g_fresh = 1;      /* the next digit starts a new number */
static double g_place;          /* 0 while typing whole numbers, else the
                                   value of the next decimal digit */
static int    g_degrees = 1;
static char   g_note[64] = "";

static const char* kKeys[ROWS][COLS] = {
    { "sin",  "cos",  "tan", "ln",  "log"  },
    { "asin", "acos", "atan", "x^2", "10^x" },
    { "sqrt", "x^y",  "1/x", "pi",  "e"    },
    { "7",    "8",    "9",   "/",   "C"   },
    { "4",    "5",    "6",   "*",   "<-"  },
    { "1",    "2",    "3",   "-",   "+/-" },
    { "0",    ".",    "=",   "+",   "DEG" },
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

/* Angles go in and come out in whatever mode is showing. */
static double to_radians(double v) { return g_degrees ? v * M_PI / 180.0 : v; }
static double from_radians(double v) { return g_degrees ? v * 180.0 / M_PI : v; }

static void complain(const char* text)
{
    snprintf(g_note, sizeof(g_note), "%s", text);
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
        /* Not an error and not a special case: dividing by zero gives an
         * infinity, which is what IEEE says and what the display can show. */
        if (g_shown == 0.0)
            complain("dividing by zero gives infinity");
        g_stored /= g_shown;
        break;
    case '^':
        g_stored = pow(g_stored, g_shown);
        if (isnan(g_stored))
            complain("no real answer for that power");
        break;
    }
    g_shown = g_stored;
}

/* The one-argument functions, applied to what is on the display. */
static void function(const char* k)
{
    g_note[0] = '\0';
    if (strcmp(k, "sqrt") == 0) {
        if (g_shown < 0.0) { complain("no real square root of a negative"); return; }
        g_shown = sqrt(g_shown);
    } else if (strcmp(k, "sin") == 0) {
        g_shown = sin(to_radians(g_shown));
    } else if (strcmp(k, "cos") == 0) {
        g_shown = cos(to_radians(g_shown));
    } else if (strcmp(k, "tan") == 0) {
        /* tan is unbounded at a right angle rather than undefined, and the
         * huge number it gives is more honest than a refusal. */
        g_shown = tan(to_radians(g_shown));
    } else if (strcmp(k, "ln") == 0) {
        if (g_shown <= 0.0) { complain("no logarithm of zero or below"); return; }
        g_shown = log(g_shown);
    } else if (strcmp(k, "log") == 0) {
        if (g_shown <= 0.0) { complain("no logarithm of zero or below"); return; }
        g_shown = log10(g_shown);
    } else if (strcmp(k, "1/x") == 0) {
        if (g_shown == 0.0) complain("dividing by zero gives infinity");
        g_shown = 1.0 / g_shown;
    } else if (strcmp(k, "asin") == 0) {
        /* Outside [-1, 1] there is no angle with that sine, and saying so
         * beats showing "not a number" and leaving them to work out why. */
        if (g_shown < -1.0 || g_shown > 1.0) { complain("sine is never outside -1 to 1"); return; }
        g_shown = from_radians(asin(g_shown));
    } else if (strcmp(k, "acos") == 0) {
        if (g_shown < -1.0 || g_shown > 1.0) { complain("cosine is never outside -1 to 1"); return; }
        g_shown = from_radians(acos(g_shown));
    } else if (strcmp(k, "atan") == 0) {
        g_shown = from_radians(atan(g_shown));
    } else if (strcmp(k, "x^2") == 0) {
        g_shown = g_shown * g_shown;
    } else if (strcmp(k, "10^x") == 0) {
        g_shown = pow(10.0, g_shown);
    }
    g_fresh = 1;
    g_place = 0.0;
}

static void press(const char* k)
{
    if (k[0] == '\0')
        return;

    if (k[0] >= '0' && k[0] <= '9' && k[1] == '\0') {
        if (g_fresh) { g_shown = 0.0; g_fresh = 0; g_place = 0.0; }
        const double digit = (double)(k[0] - '0');
        if (g_place == 0.0) {
            if (fabs(g_shown) < 1e15)
                g_shown = g_shown * 10.0 + digit;
        } else {
            /* Each decimal digit is worth a tenth of the last. Stops when the
             * place value falls below what a double can still distinguish. */
            if (g_place > 1e-15) {
                g_shown = g_shown + (g_shown < 0.0 ? -digit : digit) * g_place;
                g_place /= 10.0;
            }
        }
        return;
    }
    if (strcmp(k, ".") == 0) {
        if (g_fresh) { g_shown = 0.0; g_fresh = 0; }
        if (g_place == 0.0)
            g_place = 0.1;              /* a second point is simply ignored */
        return;
    }
    if (strcmp(k, "C") == 0) {
        g_shown = g_stored = 0.0; g_op = 0; g_fresh = 1;
        g_place = 0.0; g_note[0] = '\0';
        return;
    }
    if (strcmp(k, "<-") == 0) {
        /* Only meaningful while typing a whole number; after a result or a
         * decimal point, clearing is the honest thing. */
        if (g_place != 0.0 || g_fresh) { g_shown = 0.0; g_fresh = 1; g_place = 0.0; }
        else g_shown = trunc(g_shown / 10.0);
        return;
    }
    if (strcmp(k, "+/-") == 0) { g_shown = -g_shown; return; }
    if (strcmp(k, "pi") == 0) { g_shown = M_PI; g_fresh = 1; g_place = 0.0; return; }
    if (strcmp(k, "e") == 0)  { g_shown = M_E;  g_fresh = 1; g_place = 0.0; return; }
    if (strcmp(k, "DEG") == 0) {
        g_degrees = !g_degrees;
        snprintf(g_note, sizeof(g_note), "angles in %s",
                 g_degrees ? "degrees" : "radians");
        return;
    }
    if (strcmp(k, "=") == 0) {
        apply();
        g_op = 0; g_fresh = 1; g_place = 0.0;
        return;
    }
    if (strcmp(k, "sin") == 0 || strcmp(k, "cos") == 0 ||
        strcmp(k, "tan") == 0 || strcmp(k, "ln") == 0 ||
        strcmp(k, "log") == 0 || strcmp(k, "sqrt") == 0 ||
        strcmp(k, "1/x") == 0 || strcmp(k, "asin") == 0 ||
        strcmp(k, "acos") == 0 || strcmp(k, "atan") == 0 ||
        strcmp(k, "x^2") == 0 || strcmp(k, "10^x") == 0) {
        function(k);
        return;
    }

    /* A binary operator: finish any pending one first, so 2+3+4 works left to
     * right and 2^3^2 does too. */
    apply();
    g_stored = g_shown;
    g_op = (strcmp(k, "x^y") == 0) ? '^' : k[0];
    g_fresh = 1;
    g_place = 0.0;
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);

    wg_fill(PAD, PAD, (int)g_w - PAD * 2, 34, WG_PAPER);
    wg_bevel(PAD, PAD, (int)g_w - PAD * 2, 34, 0);

    /* %g rather than a fixed number of places: it drops trailing zeros and
     * switches to an exponent when the number would otherwise be a screenful
     * of digits. Twelve significant figures is about what fits. */
    char line[48];
    if (isnan(g_shown))
        snprintf(line, sizeof(line), "not a number");
    else if (isinf(g_shown))
        snprintf(line, sizeof(line), "%sinfinity", g_shown < 0 ? "-" : "");
    else
        snprintf(line, sizeof(line), "%.12g", g_shown);
    const int tw = (int)strlen(line) * WG_GLYPH_W;
    wg_text((int)g_w - PAD - 6 - tw, PAD + 9, line, WG_INK);

    if (g_op != 0) {
        char o[2] = { g_op, 0 };
        wg_text(PAD + 6, PAD + 9, o, WG_ACCENT);
    }
    /* The note line doubles as where the angle mode lives, so it is never a
     * question which one is in force. */
    if (g_note[0] != '\0')
        wg_text_clipped(PAD + 2, 44, g_note, WG_DIM, (int)g_w - PAD * 2 - 40);
    wg_text((int)g_w - PAD - 4 * WG_GLYPH_W, 44, g_degrees ? " deg" : " rad",
            WG_DIM);

    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) {
            if (kKeys[r][c][0] == '\0')
                continue;
            int x, y, w, h;
            key_box(r, c, &x, &y, &w, &h);
            const int down = (strcmp(kKeys[r][c], "DEG") == 0) && !g_degrees;
            wg_button(x, y, w, h, kKeys[r][c], down);
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
    win_set_min_size(id, 270, 340);
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
                    k == '*' || k == '/' || k == '.')
                    press(s);
                else if (k == '\n' || k == '\r' || k == '=') press("=");
                else if (k == 'c' || k == 'C')                press("C");
                else if (k == '\b')                           press("<-");
                else if (k == '^')                            press("x^y");
                else if (k == 's')                            press("sin");
                else if (k == 'o')                            press("cos");
                else if (k == 't')                            press("tan");
                else if (k == 'q')                            press("sqrt");
                else if (k == 'l')                            press("ln");
                else if (k == 'p')                            press("pi");
                else if (k == 'd')                            press("DEG");
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
