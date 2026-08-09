/* settings - the system's control panel.
 *
 * A sidebar of categories and a page for each, which is the arrangement every
 * control panel converges on because the alternative is one enormous scroll.
 *
 * The rule throughout is that a setting must do something. Appearance writes
 * into the window server's control block, which is the only thing that can act
 * on it; users and groups call the account syscalls; network and about report
 * what is true. Nothing here offers a switch that is merely remembered - see
 * the note on persistence under Appearance.
 */

#include <paths.h>
#include <dialog.h>
#include <display.h>
#include <audio.h>
#include <net.h>
#include <prefs.h>
#include <proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <shm.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>
#include <wproto.h>

#define PAGE_GENERAL 0
#define PAGE_APPEAR  1
#define PAGE_SOUND   2
#define PAGE_NETWORK 3
#define PAGE_USERS   4
#define PAGE_ABOUT   5
#define PAGES        6

#define SIDEBAR 118
#define ROW_H   22

static uint32_t* g_px;
static unsigned  g_w = 660, g_h = 400;
static int g_page = PAGE_GENERAL;

/* The output device, asked about once, and the volume to come back to when
 * mute is switched off. */
static struct audio_info g_audio;
static int g_vol_drag;
static int g_vol_before_mute = 70;
static char g_note[128] = "";

static const char* kPages[PAGES] = {
    "General", "Appearance", "Sound", "Network", "Users", "About"
};

/* --- the desktop's appearance ---------------------------------------------
 *
 * Reached through the same public control block a client uses to open a window.
 * Writing to it is how a setting becomes visible; bumping the generation is how
 * the server is told to look. */
static struct ws_shared* g_ws;

/* Which element the colour grid is editing. */
#define EL_DESKTOP 0
#define EL_FACE    1
#define EL_TITLE   2
#define EL_CURSOR  3
#define EL_SEL     4
#define EL_BODY    5
#define ELEMENTS   6
static const char* kElements[ELEMENTS] = {
    "desktop", "window", "title", "pointer", "selection", "body"
};

/* Whole looks rather than six separate choices. Most people want "the dark
 * one", not to pick a shadow colour; the individual controls are still there
 * for anyone who does. */
struct preset {
    const char* name;
    uint32_t desktop, face, light, shadow, title, title_text, cursor;
    uint32_t selection, body, text;
    int32_t contrast;
    uint32_t pattern;
};
static const struct preset kPresets[] = {
    { "Default",  0x008080, 0xC0C0C0, 0xFFFFFF, 0x606060, 0x000080, 0xFFFFFF,
      0xFFFFFF, 0xB0C4DE, 0xFFFFFF, 0x000000, 0, WS_PATTERN_FLAT },
    { "Slate",    0x2E3440, 0x4C566A, 0x7B88A1, 0x2B303B, 0x5E81AC, 0xECEFF4,
      0xD8DEE9, 0x5E81AC, 0x3B4252, 0xE5E9F0, 10, WS_PATTERN_GRID },
    { "Parchment",0x8B7355, 0xE8DCC0, 0xFFF8E7, 0x9A8C70, 0x6B4423, 0xFFF8E7,
      0xFFF8E7, 0xC8B48A, 0xFFFDF5, 0x2A1F14, -10, WS_PATTERN_WEAVE },
    { "Contrast", 0x000000, 0xFFFFFF, 0xFFFFFF, 0x000000, 0x000000, 0xFFFFFF,
      0xFFFF00, 0x0000FF, 0xFFFFFF, 0x000000, 60, WS_PATTERN_FLAT },
};
#define PRESETS (int)(sizeof(kPresets) / sizeof(kPresets[0]))

/* One name per WS_PATTERN_*, and the compiler is told the count so that adding
 * a pattern without naming it is a build error rather than a null pointer
 * handed to strlen - which is exactly what happened when "dither" arrived. */
/* One row of pattern buttons, sized so that all of them fit to the left of the
 * preview. Kept next to the names because changing one without the other is
 * how the row came to overlap in the first place. */
#define PATTERN_W    52
#define PATTERN_STEP 55

static const char* kPatterns[] = {
    "flat", "grid", "dots", "weave", "dither"
};
/* Deliberately unsized above, so this counts what is actually there. Written
 * the other way round - kPatterns[WS_PATTERN_COUNT] - the array is padded with
 * nulls to the declared length and the check cannot fail. */
_Static_assert(sizeof(kPatterns) / sizeof(kPatterns[0]) == WS_PATTERN_COUNT,
               "every backdrop pattern needs a name on the Appearance page");
static int g_element;

/* Where the RGB picker sits on the Appearance page, and which of its sliders
 * is being dragged. Dragging matters more than clicking here: you find a
 * colour by sweeping until it looks right, not by landing on a number. */
#define PICK_X (SIDEBAR + 90)
#define PICK_Y 118
#define PICK_W 300
static int g_pick_drag = -1;

/* Written through to the user's file as well as to the running desktop, so a
 * choice survives the session that made it. */
static void theme_changed(void)
{
    if (g_ws == 0)
        return;
    __atomic_add_fetch(&g_ws->theme.generation, 1, __ATOMIC_RELEASE);

    prefs_set_u32("theme.desktop", g_ws->theme.desktop);
    prefs_set_u32("theme.face", g_ws->theme.face);
    prefs_set_u32("theme.title", g_ws->theme.title_active);
    prefs_set_u32("theme.cursor", g_ws->theme.cursor);
    prefs_set_u32("theme.selection", g_ws->theme.selection);
    prefs_set_u32("theme.body", g_ws->theme.body);
    prefs_set_u32("theme.text", g_ws->theme.text);
    prefs_set_u32("theme.scale", g_ws->theme.text_scale);
    prefs_set_u32("theme.contrast", (unsigned)(g_ws->theme.contrast + 100));
    prefs_set_u32("theme.pattern", g_ws->theme.pattern);
    prefs_set_u32("theme.blur", g_ws->theme.blur);
    prefs_set_str("theme.wallpaper", (const char*)g_ws->theme.wallpaper);
    if (prefs_save() != 0)
        snprintf(g_note, sizeof(g_note), "changed, but could not be saved");
}

/* Put back what this user chose last time. Done once, at startup, because the
 * server starts from its own defaults and has no idea whose desktop it is. */
/* The volume the user last chose. The kernel starts every boot at its own
 * default, because it has no idea whose desktop this is going to be. */
static void apply_saved_audio(void)
{
    audio_info(&g_audio);
    if (!g_audio.present)
        return;
    const int saved = (int)prefs_get_u32("audio.volume", 80);
    audio_set_volume(saved);
    if (saved > 0)
        g_vol_before_mute = saved;
}

static void apply_saved_theme(void)
{
    if (g_ws == 0)
        return;
    prefs_load();
    g_ws->theme.desktop      = prefs_get_u32("theme.desktop", g_ws->theme.desktop);
    g_ws->theme.face         = prefs_get_u32("theme.face", g_ws->theme.face);
    g_ws->theme.title_active = prefs_get_u32("theme.title", g_ws->theme.title_active);
    g_ws->theme.cursor       = prefs_get_u32("theme.cursor", g_ws->theme.cursor);
    g_ws->theme.selection  = prefs_get_u32("theme.selection", g_ws->theme.selection);
    g_ws->theme.body       = prefs_get_u32("theme.body", g_ws->theme.body);
    g_ws->theme.text       = prefs_get_u32("theme.text", g_ws->theme.text);
    g_ws->theme.text_scale = prefs_get_u32("theme.scale", 1);
    g_ws->theme.contrast   = (int32_t)prefs_get_u32("theme.contrast", 100) - 100;
    g_ws->theme.pattern    = prefs_get_u32("theme.pattern", WS_PATTERN_DITHER);
    /* Off unless this user has asked for it: see the note in wproto.h. */
    g_ws->theme.blur       = prefs_get_u32("theme.blur", 0);
    const char* paper = prefs_get_str("theme.wallpaper", "");
    int n = 0;
    while (paper[n] != '\0' && n < 126) { g_ws->theme.wallpaper[n] = paper[n]; ++n; }
    g_ws->theme.wallpaper[n] = '\0';
    __atomic_add_fetch(&g_ws->theme.generation, 1, __ATOMIC_RELEASE);
}

/* What the sliders should start at: whatever the chosen element is already,
 * so opening the picker never throws away the current colour. */
static uint32_t element_colour(void)
{
    if (g_ws == 0)
        return 0x808080;
    switch (g_element) {
    case EL_DESKTOP: return g_ws->theme.desktop;
    case EL_FACE:    return g_ws->theme.face;
    case EL_TITLE:   return g_ws->theme.title_active;
    case EL_CURSOR:  return g_ws->theme.cursor;
    case EL_SEL:     return g_ws->theme.selection;
    case EL_BODY:    return g_ws->theme.body;
    }
    return 0x808080;
}

static void set_colour(uint32_t c)
{
    if (g_ws == 0) {
        snprintf(g_note, sizeof(g_note), "no window server to tell");
        return;
    }
    switch (g_element) {
    case EL_DESKTOP:
        g_ws->theme.desktop = c;
        /* Choosing a colour means wanting the colour, not the picture that was
         * covering it. */
        g_ws->theme.wallpaper[0] = '\0';
        break;
    case EL_FACE:
        g_ws->theme.face = c;
        break;
    case EL_TITLE:
        g_ws->theme.title_active = c;
        break;
    case EL_CURSOR:
        g_ws->theme.cursor = c;
        break;
    case EL_SEL:
        g_ws->theme.selection = c;
        break;
    case EL_BODY:
        /* Ink follows the body: light text on a light background is not a
         * choice anyone is making on purpose. */
        g_ws->theme.body = c;
        g_ws->theme.text = (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF))
                           > 3 * 128 ? 0x000000 : 0xFFFFFF;
        break;
    }
    theme_changed();
    snprintf(g_note, sizeof(g_note), "%s set", kElements[g_element]);
}

static void apply_preset(int i)
{
    if (g_ws == 0 || i < 0 || i >= PRESETS)
        return;
    const struct preset* p = &kPresets[i];
    g_ws->theme.desktop      = p->desktop;
    g_ws->theme.face         = p->face;
    g_ws->theme.light        = p->light;
    g_ws->theme.shadow       = p->shadow;
    g_ws->theme.title_active = p->title;
    g_ws->theme.title_text   = p->title_text;
    g_ws->theme.cursor       = p->cursor;
    g_ws->theme.selection    = p->selection;
    g_ws->theme.body         = p->body;
    g_ws->theme.text         = p->text;
    g_ws->theme.contrast     = p->contrast;
    g_ws->theme.pattern      = p->pattern;
    g_ws->theme.wallpaper[0] = '\0';
    theme_changed();
    snprintf(g_note, sizeof(g_note), "%s", p->name);
}

static void set_wallpaper(const char* path)
{
    if (g_ws == 0)
        return;
    int n = 0;
    while (path[n] != '\0' && n < 126) { g_ws->theme.wallpaper[n] = path[n]; ++n; }
    g_ws->theme.wallpaper[n] = '\0';
    theme_changed();
    snprintf(g_note, sizeof(g_note), "wallpaper set to %s", path);
}

/* --- users ---------------------------------------------------------------- */

static char g_uname[64], g_upass[64];
static int  g_ufield;               /* 1 name, 2 password */

struct box { int x, y, w, h; };
static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

static struct box g_f_name = { SIDEBAR + 90, 70,  180, 22 };
static struct box g_f_pass = { SIDEBAR + 90, 98,  180, 22 };
static struct box g_b_add  = { SIDEBAR + 90, 128, 96,  24 };
static struct box g_b_pass = { SIDEBAR + 194, 128, 104, 24 };
static struct box g_b_priv = { SIDEBAR + 90, 200, 200, 24 };
static struct box g_b_open = { SIDEBAR + 90, 232, 200, 24 };

static void add_user(void)
{
    if (g_uname[0] == '\0' || g_upass[0] == '\0') {
        snprintf(g_note, sizeof(g_note), "a name and a password are needed");
        return;
    }
    /* uid 0 means "allocate the next free one", which is what stops a new
     * account silently becoming an existing user. */
    if (useradd(g_uname, g_upass, 0, 0, 0) < 0)
        snprintf(g_note, sizeof(g_note), "could not create %s (root only?)", g_uname);
    else
        snprintf(g_note, sizeof(g_note), "created %s", g_uname);
    g_upass[0] = '\0';
}

static void reset_password(void)
{
    if (g_uname[0] == '\0' || g_upass[0] == '\0') {
        snprintf(g_note, sizeof(g_note), "a name and a password are needed");
        return;
    }
    /* Root may reset anyone's without knowing the old one; a user changing
     * their own goes through the same call and the kernel decides. */
    if (passwd(g_uname, "", g_upass) < 0)
        snprintf(g_note, sizeof(g_note), "refused - only root, or your own");
    else
        snprintf(g_note, sizeof(g_note), "password set for %s", g_uname);
    g_upass[0] = '\0';
}

/* "Permissions" here means what it actually means on this system: the mode of
 * the account's home directory, which is what decides whether anyone else can
 * look inside it. */
static void set_home_mode(unsigned mode)
{
    char home[128];
    snprintf(home, sizeof(home), "/home/%s", g_uname);
    if (g_uname[0] == '\0') {
        snprintf(g_note, sizeof(g_note), "name an account first");
        return;
    }
    if (chmod(home, mode) < 0)
        snprintf(g_note, sizeof(g_note), "could not change %s", home);
    else
        snprintf(g_note, sizeof(g_note), "%s is now %04o", home, mode);
}

/* --- drawing --------------------------------------------------------------- */

static void field(const struct box* b, const char* label, const char* text,
                  int focused, int secret)
{
    wg_text(SIDEBAR + 14, b->y + 3, label, WG_DIM);
    wg_fill(b->x, b->y, b->w, b->h, WG_PAPER);
    wg_bevel(b->x, b->y, b->w, b->h, 0);
    if (secret) {
        for (unsigned k = 0; k < strlen(text) && (int)k * 10 < b->w - 12; ++k)
            wg_fill(b->x + 6 + (int)k * 10, b->y + b->h / 2 - 2, 5, 5, WG_INK);
    } else {
        wg_text_clipped(b->x + 5, b->y + 3, text, WG_INK, b->w - 10);
    }
    if (focused)
        wg_fill(b->x + 2, b->y + 3, 1, b->h - 6, WG_ACCENT);
}

static void kv(int y, const char* k, const char* v)
{
    wg_text(SIDEBAR + 14, y, k, WG_DIM);
    wg_text_clipped(SIDEBAR + 130, y, v, WG_INK, (int)g_w - SIDEBAR - 145);
}

static void draw_general(void)
{
    char name[64] = "?", line[96], cwd[128] = "";
    username(getuid(), name);
    getcwd(cwd, sizeof(cwd));
    wg_text(SIDEBAR + 14, 16, "General", WG_INK);
    wg_fill(SIDEBAR + 14, 34, (int)g_w - SIDEBAR - 28, 1, WG_DIM);

    kv(50, "user", name);
    snprintf(line, sizeof(line), "%u", getuid());
    kv(70, "uid", line);
    kv(90, "directory", cwd);

    struct fb_info fb;
    if (fb_info(&fb) == 0)
        snprintf(line, sizeof(line), "%ux%u, %u bpp", fb.width, fb.height,
                 fb.bits_per_pixel);
    else
        snprintf(line, sizeof(line), "none");
    kv(110, "screen", line);

    struct mem_info m;
    if (mem_info(&m) == 0) {
        snprintf(line, sizeof(line), "%llu KiB of %llu KiB used",
                 (unsigned long long)(m.used / 1024),
                 (unsigned long long)(m.usable / 1024));
        kv(130, "memory", line);
    }
    struct proc_info procs[64];
    const int n = proc_list(procs, 64);
    snprintf(line, sizeof(line), "%d", n < 0 ? 0 : n);
    kv(150, "tasks", line);

    wg_text_clipped(SIDEBAR + 14, 186,
                    "the desktop is the window server; log out to restart it",
                    WG_DIM, (int)g_w - SIDEBAR - 28);
}

static void draw_appearance(void)
{
    wg_text(SIDEBAR + 14, 16, "Appearance", WG_INK);
    wg_fill(SIDEBAR + 14, 34, (int)g_w - SIDEBAR - 28, 1, WG_DIM);

    wg_text(SIDEBAR + 14, 42, "theme", WG_DIM);
    for (int i = 0; i < PRESETS; ++i)
        wg_button(SIDEBAR + 90 + i * 78, 38, 74, 22, kPresets[i].name, 0);

    wg_text(SIDEBAR + 14, 70, "element", WG_DIM);
    for (int i = 0; i < ELEMENTS; ++i)
        wg_button(SIDEBAR + 90 + (i % 3) * 78, 66 + (i / 3) * 24, 74, 22,
                  kElements[i], g_element == i);

    wg_text(SIDEBAR + 14, 122, "colour", WG_DIM);
    wg_rgb_draw(PICK_X, PICK_Y, PICK_W, element_colour());

    wg_text(SIDEBAR + 14, 196, "text", WG_DIM);
    wg_button(SIDEBAR + 90, 192, 74, 22, "normal",
              g_ws && g_ws->theme.text_scale != 2);
    wg_button(SIDEBAR + 168, 192, 74, 22, "large",
              g_ws && g_ws->theme.text_scale == 2);

    wg_text(SIDEBAR + 14, 224, "contrast", WG_DIM);
    wg_button(SIDEBAR + 90, 220, 34, 22, "-", 0);
    wg_button(SIDEBAR + 126, 220, 34, 22, "+", 0);
    {
        char c[24];
        snprintf(c, sizeof(c), "%d", g_ws ? (int)g_ws->theme.contrast : 0);
        wg_text(SIDEBAR + 166, 223, c, WG_INK);
    }

    /* Narrower than they look like they should be, and the preview is further
     * right than it was: five buttons and a 150-pixel preview both wanted the
     * same strip. They overlapped by fourteen pixels even with four. */
    wg_text(SIDEBAR + 14, 252, "pattern", WG_DIM);
    for (int i = 0; i < WS_PATTERN_COUNT; ++i)
        wg_button(SIDEBAR + 90 + i * PATTERN_STEP, 248, PATTERN_W, 22,
                  kPatterns[i], g_ws && (int)g_ws->theme.pattern == i);

    /* A live preview, so a choice can be judged before it is made. */
    wg_text(SIDEBAR + 366, 196, "preview", WG_DIM);
    const int px = SIDEBAR + 366, py = 214;
    wg_fill(px, py, 150, 60, g_ws ? g_ws->theme.desktop : 0x008080);
    wg_fill(px + 12, py + 10, 110, 40, g_ws ? g_ws->theme.face : 0xC0C0C0);
    wg_bevel(px + 12, py + 10, 110, 40, 1);
    wg_fill(px + 15, py + 13, 104, 12, g_ws ? g_ws->theme.title_active : 0x000080);
    wg_bevel(px, py, 150, 60, 0);

    /* Not a matter of taste, which is why it is not part of a preset: on a
     * machine without graphics acceleration the glass is what makes the
     * desktop feel slow, and this is the switch that gets it back. */
    wg_text(SIDEBAR + 14, 280, "window backdrop", WG_DIM);
    wg_button(SIDEBAR + 156, 276, 74, 22, "opaque",
              g_ws && g_ws->theme.blur == 0);
    wg_button(SIDEBAR + 234, 276, 74, 22, "blurred",
              g_ws && g_ws->theme.blur != 0);

    wg_text(SIDEBAR + 14, 310, "wallpaper", WG_DIM);
    wg_button(SIDEBAR + 90, 306, 130, 24, "Choose a PNG...", 0);
    wg_button(SIDEBAR + 228, 306, 96, 24, "Remove", 0);
    if (g_ws != 0 && g_ws->theme.wallpaper[0] != '\0')
        wg_text_clipped(SIDEBAR + 90, 334, (const char*)g_ws->theme.wallpaper,
                        WG_INK, (int)g_w - SIDEBAR - 105);
    else
        wg_text(SIDEBAR + 90, 334, "none - the pattern shows", WG_DIM);

    wg_text_clipped(SIDEBAR + 14, 358,
                    "saved to ~/.leahrc and restored when settings next starts",
                    WG_DIM, (int)g_w - SIDEBAR - 28);
}

/* --- sound ----------------------------------------------------------------
 *
 * One control that matters and a way to hear the effect of moving it. Volume
 * without a test tone is a setting you have to go and find something else to
 * check, which is how a volume slider ends up being adjusted by trial against
 * whatever happens to be playing.
 */
#define VOL_X (SIDEBAR + 90)
#define VOL_Y 62
#define VOL_W 260
/* A quarter-second of A above middle C, generated here rather than launched as
 * a program: a test tone that takes a fork and an exec to make a sound is
 * testing the wrong thing. */
static void test_tone(void)
{
    if (!g_audio.present) {
        snprintf(g_note, sizeof(g_note), "no output device");
        return;
    }
    static short chunk[512];
    const long frames = AUDIO_RATE / 4;
    const long period = AUDIO_RATE / 440;
    long done = 0;
    while (done < frames) {
        long n = 0;
        while (n < 256 && done + n < frames) {
            /* Fade the ends, or the start and stop are louder than the note. */
            const long i = done + n;
            const long fade = AUDIO_RATE / 100;
            long gain = 256;
            if (i < fade)              gain = i * 256 / fade;
            else if (i > frames - fade) gain = (frames - i) * 256 / fade;
            if (gain < 0) gain = 0;
            const short v = (short)((((i % period) < period / 2) ? 7000 : -7000)
                                    * gain / 256);
            chunk[n * 2] = chunk[n * 2 + 1] = v;
            ++n;
        }
        long off = 0;
        while (off < n) {
            const long took = audio_play(&chunk[off * 2], (n - off) * 2);
            if (took <= 0) { msleep(5); continue; }
            off += took / 2;
        }
        done += n;
    }
    audio_flush();
    snprintf(g_note, sizeof(g_note), "played a 440 Hz tone");
}

static void set_audio_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    audio_set_volume(percent);
    prefs_set_u32("audio.volume", (uint32_t)percent);
    prefs_save();
}

static void draw_sound(void)
{
    wg_text(SIDEBAR + 14, 16, "Sound", WG_INK);
    wg_fill(SIDEBAR + 14, 34, (int)g_w - SIDEBAR - 28, 1, WG_DIM);

    if (!g_audio.present) {
        wg_text(SIDEBAR + 14, 62, "no audio device found", WG_DIM);
        return;
    }

    const int vol = audio_volume();
    wg_text(SIDEBAR + 14, VOL_Y + 4, "volume", WG_DIM);
    wg_slider_draw(VOL_X, VOL_Y, VOL_W, vol, 100);
    {
        char v[16];
        snprintf(v, sizeof(v), "%d%%", vol);
        wg_text(VOL_X + VOL_W + 12, VOL_Y + 4, v, WG_INK);
    }

    wg_button(VOL_X, VOL_Y + 30, 74, 22, vol == 0 ? "Unmute" : "Mute", vol == 0);
    wg_button(VOL_X + 82, VOL_Y + 30, 90, 22, "Test tone", 0);

    wg_text(SIDEBAR + 14, VOL_Y + 74, "output", WG_DIM);
    wg_text_clipped(VOL_X, VOL_Y + 74, g_audio.name, WG_INK,
                    (int)g_w - VOL_X - 20);

    wg_text(SIDEBAR + 14, VOL_Y + 96, "format", WG_DIM);
    {
        char f[64];
        snprintf(f, sizeof(f), "%u Hz, %u channels, 16-bit",
                 g_audio.rate, g_audio.channels);
        wg_text(VOL_X, VOL_Y + 96, f, WG_INK);
    }

    wg_text_clipped(SIDEBAR + 14, VOL_Y + 130,
                    "the volume is saved to ~/.leahrc and restored at login",
                    WG_DIM, (int)g_w - SIDEBAR - 28);
}

static void draw_network(void)
{
    wg_text(SIDEBAR + 14, 16, "Network", WG_INK);
    wg_fill(SIDEBAR + 14, 34, (int)g_w - SIDEBAR - 28, 1, WG_DIM);

    struct netinfo ni;
    char line[64];
    if (netinfo(&ni) != 0) {
        wg_text(SIDEBAR + 14, 56, "no interface is configured", WG_DIM);
        return;
    }
    snprintf(line, sizeof(line), "%u.%u.%u.%u", (ni.ip >> 24) & 0xFF,
             (ni.ip >> 16) & 0xFF, (ni.ip >> 8) & 0xFF, ni.ip & 0xFF);
    kv(56, "address", line);
    snprintf(line, sizeof(line), "%u.%u.%u.%u", (ni.netmask >> 24) & 0xFF,
             (ni.netmask >> 16) & 0xFF, (ni.netmask >> 8) & 0xFF, ni.netmask & 0xFF);
    kv(76, "netmask", line);
    snprintf(line, sizeof(line), "%u.%u.%u.%u", (ni.gateway >> 24) & 0xFF,
             (ni.gateway >> 16) & 0xFF, (ni.gateway >> 8) & 0xFF, ni.gateway & 0xFF);
    kv(96, "gateway", line);
    snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x",
             ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    kv(116, "hardware", line);
    wg_text_clipped(SIDEBAR + 14, 150,
                    "the address is fixed at boot; there is no DHCP client yet",
                    WG_DIM, (int)g_w - SIDEBAR - 28);
}

static void draw_users(void)
{
    wg_text(SIDEBAR + 14, 16, "Users and groups", WG_INK);
    wg_fill(SIDEBAR + 14, 34, (int)g_w - SIDEBAR - 28, 1, WG_DIM);

    field(&g_f_name, "account", g_uname, g_ufield == 1, 0);
    field(&g_f_pass, "password", g_upass, g_ufield == 2, 1);
    wg_button(g_b_add.x, g_b_add.y, g_b_add.w, g_b_add.h, "Create", 0);
    wg_button(g_b_pass.x, g_b_pass.y, g_b_pass.w, g_b_pass.h, "Set password", 0);

    wg_text(SIDEBAR + 14, 176, "permissions", WG_DIM);
    wg_text_clipped(SIDEBAR + 90, 176, "who may look inside the home directory",
                    WG_DIM, (int)g_w - SIDEBAR - 105);
    wg_button(g_b_priv.x, g_b_priv.y, g_b_priv.w, g_b_priv.h,
              "Private (0700)", 0);
    wg_button(g_b_open.x, g_b_open.y, g_b_open.w, g_b_open.h,
              "Readable by all (0755)", 0);

    wg_text_clipped(SIDEBAR + 14, 268,
                    getuid() == 0 ? "you are root: you may change any account"
                                  : "only root may create accounts",
                    WG_DIM, (int)g_w - SIDEBAR - 28);
}

static void draw_about(void)
{
    wg_text(SIDEBAR + 14, 16, "About", WG_INK);
    wg_fill(SIDEBAR + 14, 34, (int)g_w - SIDEBAR - 28, 1, WG_DIM);

    wg_text(SIDEBAR + 14, 54, "leahOS", WG_INK);
    kv(78,  "kind", "a UNIX-like system for x86-64");
    kv(98,  "built", "from scratch: no third-party bootloader,");
    kv(118, "", "no libc, no runtime");
    kv(142, "kernel", "NASM and C++23, higher half at -2 GiB");
    kv(162, "storage", "ext2/3/4 read and write, AHCI, USB");
    kv(182, "network", "e1000, IPv4, ARP, ICMP, UDP, DNS, TCP");
    kv(202, "desktop", "a window server in userland on shared memory");

    struct mem_info m;
    char line[64];
    if (mem_info(&m) == 0) {
        snprintf(line, sizeof(line), "%llu MiB usable",
                 (unsigned long long)(m.usable / (1024 * 1024)));
        kv(222, "memory", line);
    }
    wg_text_clipped(SIDEBAR + 14, 254, "README.md is the long version",
                    WG_DIM, (int)g_w - SIDEBAR - 28);
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);

    /* The sidebar, sunken so it reads as a place rather than a row of buttons. */
    wg_fill(0, 0, SIDEBAR, (int)g_h, 0xB0B0B0);
    wg_bevel(0, 0, SIDEBAR, (int)g_h, 0);
    for (int i = 0; i < PAGES; ++i) {
        const int y = 10 + i * ROW_H;
        if (i == g_page) {
            wg_fill(4, y, SIDEBAR - 8, ROW_H - 2, WG_FACE);
            wg_bevel(4, y, SIDEBAR - 8, ROW_H - 2, 1);
        }
        wg_text(14, y + 3, kPages[i], WG_INK);
    }

    switch (g_page) {
    case PAGE_GENERAL: draw_general();    break;
    case PAGE_APPEAR:  draw_appearance(); break;
    case PAGE_SOUND:   draw_sound();      break;
    case PAGE_NETWORK: draw_network();    break;
    case PAGE_USERS:   draw_users();      break;
    default:           draw_about();      break;
    }

    wg_fill(0, (int)g_h - 20, (int)g_w, 20, WG_FACE);
    wg_text_clipped(8, (int)g_h - 18, g_note, WG_DIM, (int)g_w - 16);
}

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 150;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 100;
    if (wg_font() != 0)
        return 1;

    /* The desktop's own control block, which is public precisely so that a
     * client like this can find it. */
    const int cid = shm_open(WS_CONTROL_KEY, 0, 0);
    if (cid >= 0)
        g_ws = (struct ws_shared*)shm_map(cid);
    apply_saved_theme();
    apply_saved_audio();

    const int id = win_create(wx, wy, g_w, g_h, "Settings");
    if (id < 0) {
        printf("settings: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 620, 360);
    wg_target(g_px, g_w, g_h);
    draw();
    win_present(id);

    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            if (dlg_active() && e.type != WIN_EVENT_RESIZE) {
                if (dlg_event(&e) == DLG_ACCEPT)
                    set_wallpaper(dlg_path());
                draw();
                dlg_draw((int)g_w, (int)g_h);
                win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && g_vol_drag) {
                set_audio_volume(wg_slider_value(VOL_X, VOL_W, e.x, 100));
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && g_pick_drag >= 0) {
                set_colour(wg_rgb_move(element_colour(), g_pick_drag,
                                       PICK_X, PICK_W, e.x));
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                g_pick_drag = -1;
                g_vol_drag = 0;
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                if (e.x < SIDEBAR) {
                    const int i = (e.y - 10) / ROW_H;
                    if (i >= 0 && i < PAGES) { g_page = i; g_note[0] = '\0'; }
                } else if (g_page == PAGE_SOUND && g_audio.present) {
                    if (wg_slider_hit(VOL_X, VOL_Y, VOL_W, e.x, e.y)) {
                        g_vol_drag = 1;
                        set_audio_volume(
                            wg_slider_value(VOL_X, VOL_W, e.x, 100));
                    } else if (e.y >= VOL_Y + 30 && e.y < VOL_Y + 52) {
                        if (e.x >= VOL_X && e.x < VOL_X + 74) {
                            /* Mute remembers where it came back to, so it is a
                             * toggle rather than a trip to zero and a guess. */
                            const int now = audio_volume();
                            if (now == 0) {
                                set_audio_volume(g_vol_before_mute);
                            } else {
                                g_vol_before_mute = now;
                                set_audio_volume(0);
                            }
                        } else if (e.x >= VOL_X + 82 && e.x < VOL_X + 172) {
                            test_tone();
                        }
                    }
                } else if (g_page == PAGE_APPEAR) {
                    for (int i = 0; i < PRESETS; ++i)
                        if (e.x >= SIDEBAR + 90 + i * 78 &&
                            e.x < SIDEBAR + 164 + i * 78 &&
                            e.y >= 38 && e.y < 60)
                            apply_preset(i);
                    for (int i = 0; i < ELEMENTS; ++i) {
                        const int bx = SIDEBAR + 90 + (i % 3) * 78;
                        const int by = 66 + (i / 3) * 24;
                        if (e.x >= bx && e.x < bx + 74 &&
                            e.y >= by && e.y < by + 22)
                            g_element = i;
                    }
                    g_pick_drag = wg_rgb_hit(PICK_X, PICK_Y, PICK_W, e.x, e.y);
                    if (g_pick_drag >= 0)
                        set_colour(wg_rgb_move(element_colour(), g_pick_drag,
                                               PICK_X, PICK_W, e.x));
                    if (g_ws != 0 && e.y >= 192 && e.y < 214) {
                        if (e.x >= SIDEBAR + 90 && e.x < SIDEBAR + 164)
                            { g_ws->theme.text_scale = 1; theme_changed(); }
                        else if (e.x >= SIDEBAR + 168 && e.x < SIDEBAR + 242)
                            { g_ws->theme.text_scale = 2; theme_changed(); }
                    }
                    if (g_ws != 0 && e.y >= 220 && e.y < 242) {
                        int c = g_ws->theme.contrast;
                        if (e.x >= SIDEBAR + 90 && e.x < SIDEBAR + 124) c -= 10;
                        else if (e.x >= SIDEBAR + 126 && e.x < SIDEBAR + 160) c += 10;
                        if (c < -100) c = -100;
                        if (c > 100) c = 100;
                        if (c != g_ws->theme.contrast)
                            { g_ws->theme.contrast = c; theme_changed(); }
                    }
                    if (g_ws != 0 && e.y >= 248 && e.y < 270) {
                        for (int i = 0; i < WS_PATTERN_COUNT; ++i)
                            if (e.x >= SIDEBAR + 90 + i * PATTERN_STEP &&
                                e.x < SIDEBAR + 90 + PATTERN_W +
                                      i * PATTERN_STEP)
                                { g_ws->theme.pattern = (uint32_t)i;
                                  theme_changed(); }
                    }
                    if (g_ws != 0 && e.y >= 276 && e.y < 298) {
                        if (e.x >= SIDEBAR + 156 && e.x < SIDEBAR + 230)
                            { g_ws->theme.blur = 0; theme_changed(); }
                        else if (e.x >= SIDEBAR + 234 && e.x < SIDEBAR + 308)
                            { g_ws->theme.blur = 1; theme_changed(); }
                    }
                    if (e.x >= SIDEBAR + 90 && e.x < SIDEBAR + 220 &&
                        e.y >= 306 && e.y < 330)
                        dlg_save(PATH_WALLPAPERS, "");  /* a picker, reused */
                    else if (e.x >= SIDEBAR + 228 && e.x < SIDEBAR + 324 &&
                             e.y >= 306 && e.y < 330)
                        set_wallpaper("");
                } else if (g_page == PAGE_USERS) {
                    if (inside(&g_f_name, e.x, e.y))      g_ufield = 1;
                    else if (inside(&g_f_pass, e.x, e.y)) g_ufield = 2;
                    else if (inside(&g_b_add, e.x, e.y))  add_user();
                    else if (inside(&g_b_pass, e.x, e.y)) reset_password();
                    else if (inside(&g_b_priv, e.x, e.y)) set_home_mode(0700);
                    else if (inside(&g_b_open, e.x, e.y)) set_home_mode(0755);
                    else g_ufield = 0;
                }
            } else if (e.type == WIN_EVENT_KEY &&
                       (e.key == WIN_KEY_UP || e.key == WIN_KEY_DOWN)) {
                /* The sidebar is a list, so it walks like one. */
                const int to = g_page + (e.key == WIN_KEY_DOWN ? 1 : -1);
                if (to >= 0 && to < PAGES) { g_page = to; g_note[0] = '\0'; }
            } else if (e.type == WIN_EVENT_KEY && g_page == PAGE_USERS) {
                char* f = g_ufield == 1 ? g_uname : g_ufield == 2 ? g_upass : 0;
                if (f == 0)
                    continue;
                const char c = (char)e.key;
                unsigned n = (unsigned)strlen(f);
                if (c == '\b' || c == 0x7F) { if (n > 0) f[n - 1] = '\0'; }
                else if (c == '\n' || c == '\r') g_ufield = g_ufield == 1 ? 2 : 0;
                else if ((unsigned char)c >= 32 && n + 1 < 63)
                    { f[n] = c; f[n + 1] = '\0'; }
            } else {
                continue;
            }
            draw();
            dlg_draw((int)g_w, (int)g_h);
            win_present(id);
        }
        msleep(15);
    }
}
