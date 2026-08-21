/* Grab - a picture of the screen.
 *
 * `screenshot` already writes one, and writing one is not the hard part: the
 * hard part is that the moment you want a picture of is usually a moment that
 * a terminal window is in the way of. So this waits, and while it waits it is
 * out of the way itself - the window hides, the countdown runs, the shutter
 * goes, and the window comes back holding what it caught.
 *
 * The framebuffer holds whatever the compositor last put there, so reading it
 * is the whole capture. Nothing has to cooperate and nothing is redrawn.
 */

#include <app.h>
#include <ui.h>
#include <display.h>
#include <image.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

static struct app g_app;
static struct ui_view* g_delay;
static struct ui_view* g_note;
static struct ui_view* g_preview;
static struct ui_view* g_save;

/* What was caught, at the screen's size, and a shrunken copy to show. */
static uint32_t* g_shot;
static unsigned  g_shot_w, g_shot_h;
static char g_message[160];

/* The countdown, in ticks of the application's clock. -1 when not counting. */
static int g_ticks_left = -1;
static int g_hidden;

static void say(const char* text)
{
    snprintf(g_message, sizeof(g_message), "%s", text);
    ui_set_text(g_note, g_message);
}

static void capture(void)
{
    struct fb_info fb;
    if (fb_info(&fb) != 0 || fb.width == 0 || fb.height == 0) {
        say("there is no screen to photograph");
        return;
    }
    if (fb.bits_per_pixel != 32) {
        say("the screen is not 32 bits per pixel");
        return;
    }
    const unsigned char* screen = (const unsigned char*)fb_map();
    if (screen == 0) {
        /* Mapping the framebuffer is root's, which is the same rule that stops
         * any process drawing over everyone's windows. */
        say("only root may photograph the screen");
        return;
    }

    free(g_shot);
    g_shot = (uint32_t*)malloc((unsigned long)fb.width * fb.height * 4);
    if (g_shot == 0) {
        say("there was no room for a picture that size");
        return;
    }
    g_shot_w = fb.width;
    g_shot_h = fb.height;
    for (unsigned y = 0; y < fb.height; ++y) {
        const uint32_t* row = (const uint32_t*)(screen + (unsigned long)y * fb.pitch);
        for (unsigned x = 0; x < fb.width; ++x)
            /* Opaque, explicitly: what is on the screen has no transparency
             * left in it, and a zero alpha byte means "not there" to
             * everything downstream. */
            g_shot[(unsigned long)y * fb.width + x] = 0xFF000000u | row[x];
    }
    char note[160];
    snprintf(note, sizeof(note), "caught %ux%u - choose where to keep it",
             g_shot_w, g_shot_h);
    say(note);
}

/* --- the preview -------------------------------------------------------------- */

static void draw_preview(struct ui_view* v, void* user)
{
    (void)user;
    const struct ui_rect f = v->frame;
    wg_container(f.x, f.y, f.w, f.h, WG_RADIUS);
    if (g_shot == 0) {
        const char* text = "nothing caught yet";
        wg_text(f.x + (f.w - wg_text_width(text)) / 2,
                f.y + f.h / 2 - WG_GLYPH_H / 2, text, WG_DIM);
        return;
    }
    /* Fitted rather than stretched: a screen is not the shape of this box, and
     * a picture squashed to fit is a picture of a different screen. */
    int w = f.w - 12, h = f.h - 12;
    if ((int)(g_shot_w * (unsigned)h) > (int)(g_shot_h * (unsigned)w))
        h = (int)((unsigned long)w * g_shot_h / g_shot_w);
    else
        w = (int)((unsigned long)h * g_shot_w / g_shot_h);
    wg_icon_scaled(f.x + (f.w - w) / 2, f.y + (f.h - h) / 2,
                   g_shot, (int)g_shot_w, (int)g_shot_h, w, h);
}

/* --- what the buttons do ------------------------------------------------------- */

static const char* delay_name(void* user, int i)
{
    (void)user;
    static const char* const kNames[] = { "now", "after 3 s", "after 5 s",
                                          "after 10 s" };
    return (i >= 0 && i < 4) ? kNames[i] : "";
}
static const int kDelaySeconds[4] = { 0, 3, 5, 10 };

static void on_capture(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    const int which = g_delay->selected >= 0 ? g_delay->selected : 0;
    const int seconds = kDelaySeconds[which];
    if (seconds == 0) {
        /* Still a moment, so the press that asked for it is not in the
         * picture: the window has to be gone before the shutter goes. */
        win_hide(g_app.id);
        g_hidden = 1;
        g_ticks_left = 4;
        say("...");
        return;
    }
    win_hide(g_app.id);
    g_hidden = 1;
    g_ticks_left = seconds * 4;         /* the tick is four to the second */
    say("counting down");
}

static void on_saved(struct app* a, int result)
{
    if (!result || g_shot == 0)
        return;
    const char* path = app_sheet_path(a);
    if (img_write_png(path, g_shot, g_shot_w, g_shot_h) != 0) {
        say("that picture could not be written");
        return;
    }
    char note[160];
    snprintf(note, sizeof(note), "kept as %s", path);
    say(note);
}

static void on_save(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    if (g_shot == 0) {
        say("catch something first");
        return;
    }
    app_sheet_save(&g_app, "/root", "screen.png");
}

static int on_tick(struct app* a)
{
    if (g_ticks_left < 0)
        return 0;
    if (--g_ticks_left > 0) {
        if (g_ticks_left % 4 == 0) {
            char note[64];
            snprintf(note, sizeof(note), "%d...", g_ticks_left / 4);
            say(note);
            return 1;
        }
        return 0;
    }
    g_ticks_left = -1;
    capture();
    if (g_hidden) {
        win_show(a->id);
        g_hidden = 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 10, 8);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 0, 8);
    ui_grow(ui_size(bar, 0, 26), 0);
    ui_grow(ui_label(bar, "Take a picture"), 0);
    g_delay = ui_grow(ui_size(ui_popup(bar, delay_name, 4, 0), 116, 24), 0);
    g_delay->selected = 1;
    ui_grow(ui_size(ui_button(bar, "Capture", on_capture, 0), 92, 24), 0);
    ui_spacer(bar);
    g_save = ui_grow(ui_size(ui_button(bar, "Save...", on_save, 0), 88, 24), 0);

    g_preview = ui_grow(ui_custom(root, draw_preview, 0), 1);
    g_note = ui_grow(ui_size(ui_label(root, ""), 0, 20), 0);

    g_app.title = "Grab";
    g_app.width = 560; g_app.height = 430;
    g_app.min_width = 420; g_app.min_height = 300;
    g_app.root = root;
    g_app.tick_ms = 250;
    g_app.tick = on_tick;
    g_app.sheet_done = on_saved;
    say("the window gets out of the way while it counts");
    return app_run(&g_app, argc, argv);
}
