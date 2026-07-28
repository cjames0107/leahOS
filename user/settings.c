/* settings - who you are, what the machine is, and the one setting a user can
 * actually change here: their own password.
 *
 * Everything else this desktop has - window positions, the colour of the
 * background - lives in a running process and nowhere else, because there is no
 * store to persist it to. Rather than offer switches that forget themselves the
 * moment the session ends, this shows what is true and changes what it can.
 */

#include <display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

static uint32_t* g_px;
static unsigned  g_w = 380, g_h = 280;

/* The password flow: two entries that have to agree, so a typo cannot lock an
 * account out. Neither is ever drawn back to the screen. */
static char g_first[64], g_second[64];
static int  g_field;            /* 0 none, 1 first, 2 second */
static char g_note[80] = "";

struct box { int x, y, w, h; };
static struct box g_f1   = { 150, 150, 200, 22 };
static struct box g_f2   = { 150, 178, 200, 22 };
static struct box g_go   = { 150, 208, 96,  24 };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

static void row(int y, const char* label, const char* value)
{
    wg_text(16, y, label, WG_DIM);
    wg_text(150, y, value, WG_INK);
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);
    wg_text(16, 12, "System", WG_INK);
    wg_fill(16, 30, (int)g_w - 32, 1, WG_DIM);

    char name[64] = "?";
    username(getuid(), name);
    char line[64];

    row(40, "user", name);
    snprintf(line, sizeof(line), "%d", getuid());
    row(58, "uid", line);
    char cwd[128] = "";
    getcwd(cwd, sizeof(cwd));
    row(76, "directory", cwd);

    struct fb_info fb;
    if (fb_info(&fb) == 0)
        snprintf(line, sizeof(line), "%ux%u, %u bpp", fb.width, fb.height,
                 fb.bits_per_pixel);
    else
        snprintf(line, sizeof(line), "no framebuffer");
    row(94, "screen", line);

    wg_text(16, 122, "Change password", WG_INK);
    wg_fill(16, 140, (int)g_w - 32, 1, WG_DIM);

    wg_text(16, g_f1.y + 3, "new", WG_DIM);
    wg_text(16, g_f2.y + 3, "again", WG_DIM);
    for (int i = 0; i < 2; ++i) {
        const struct box* b = i == 0 ? &g_f1 : &g_f2;
        const char* text = i == 0 ? g_first : g_second;
        wg_fill(b->x, b->y, b->w, b->h, WG_PAPER);
        wg_bevel(b->x, b->y, b->w, b->h, 0);
        /* Shown as dots: a password on screen is a password over someone's
         * shoulder. */
        for (unsigned k = 0; k < strlen(text) && (int)k * 10 < b->w - 12; ++k)
            wg_fill(b->x + 6 + (int)k * 10, b->y + b->h / 2 - 2, 5, 5, WG_INK);
        if (g_field == i + 1)
            wg_fill(b->x + 2, b->y + 3, 1, b->h - 6, WG_ACCENT);
    }
    wg_button(g_go.x, g_go.y, g_go.w, g_go.h, "Set", 0);
    wg_text_clipped(16, 244, g_note, WG_DIM, (int)g_w - 32);
}

static void commit(void)
{
    if (g_first[0] == '\0') {
        snprintf(g_note, sizeof(g_note), "a password cannot be empty");
        return;
    }
    if (strcmp(g_first, g_second) != 0) {
        snprintf(g_note, sizeof(g_note), "the two entries do not match");
        return;
    }
    char name[64] = "";
    username(getuid(), name);
    /* The old password is not asked for: this session already proved who it is
     * at the login prompt, and the kernel enforces that a user may only change
     * their own. */
    if (passwd(name, "", g_first) < 0)
        snprintf(g_note, sizeof(g_note), "the system refused the change");
    else
        snprintf(g_note, sizeof(g_note), "password changed for %s", name);
    g_first[0] = g_second[0] = '\0';
}

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 260;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 200;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Settings");
    if (id < 0) {
        printf("settings: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 360, 270);
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
                if (inside(&g_f1, e.x, e.y))      g_field = 1;
                else if (inside(&g_f2, e.x, e.y)) g_field = 2;
                else if (inside(&g_go, e.x, e.y)) commit();
                else g_field = 0;
            } else if (e.type == WIN_EVENT_KEY) {
                char* f = g_field == 1 ? g_first : g_field == 2 ? g_second : 0;
                if (f == 0)
                    continue;
                const char c = (char)e.key;
                unsigned n = (unsigned)strlen(f);
                if (c == '\b' || c == 0x7F) {
                    if (n > 0) f[n - 1] = '\0';
                } else if (c == '\n' || c == '\r') {
                    if (g_field == 1) g_field = 2; else commit();
                } else if ((unsigned char)c >= 32 && n + 1 < 63) {
                    f[n] = c; f[n + 1] = '\0';
                }
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
