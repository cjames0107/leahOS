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

#include <app.h>
#include <ui.h>
#include <paths.h>
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

static int g_page = PAGE_GENERAL;

/* The output device, asked about once, and the volume to come back to when
 * mute is switched off. */
static struct audio_info g_audio;
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

/* Which of the two looks is on. Remembered rather than worked out from the
 * colours: a theme read back from ~/.leahrc is a set of numbers, and asking
 * "is this the dark one" of a pile of numbers is a guess. */
static int g_mode;

/* Two looks, and no way to pick a shadow colour.
 *
 * There used to be six elements and three sliders, so a person could set the
 * pointer to lime and the title bar to maroon. That is a lot of interface for
 * a choice nobody makes, and every combination it allows has to look like this
 * system - which most of them did not. Light or dark is the choice people
 * actually have, and the rest follows from it.
 *
 * The presets remain as the definition of those two looks rather than as a
 * menu: light is the first and dark is the second, and nothing else is
 * reachable from the interface. */
struct preset {
    const char* name;
    uint32_t desktop, face, light, shadow, title, title_text, cursor;
    uint32_t selection, body, text;
    int32_t contrast;
    uint32_t pattern;
};
static const struct preset kPresets[] = {
    { "Light", 0x8894A8, 0xF2F4F7, 0xFFFFFF, 0x9AA3AE, 0xF2F4F7, 0x18202B,
      0xFFFFFF, 0x2C6BED, 0xFFFFFF, 0x18202B, 0, WS_PATTERN_FLAT },
    /* Dark, and dark all the way through: a window face that is dark with ink
     * that is still black is not a dark mode, it is an unreadable light one. */
    { "Dark",  0x1B2028, 0x272C34, 0x3A414D, 0x14181E, 0x272C34, 0xE8ECF2,
      0xE8ECF2, 0x3E7BF0, 0x1E232A, 0xE8ECF2, 10, WS_PATTERN_FLAT },
};
#define PRESETS (int)(sizeof(kPresets) / sizeof(kPresets[0]))
#define MODE_LIGHT 0
#define MODE_DARK  1

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
    prefs_set_u32("theme.mode", (unsigned)g_mode);
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
    g_mode = (int)prefs_get_u32("theme.mode", MODE_LIGHT);
    /* A picture rather than a flat colour, because a desktop with nothing on
     * it looks like something failed to load. Whatever the person chose wins;
     * this is only what is there before anybody chooses. */
    const char* paper = prefs_get_str("theme.wallpaper",
                                      "/usr/share/wallpapers/town.png");
    int n = 0;
    while (paper[n] != '\0' && n < 126) { g_ws->theme.wallpaper[n] = paper[n]; ++n; }
    g_ws->theme.wallpaper[n] = '\0';
    __atomic_add_fetch(&g_ws->theme.generation, 1, __ATOMIC_RELEASE);
}



static void apply_preset(int i)
{
    g_mode = i;
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

/* --- sound ----------------------------------------------------------------
 *
 * One control that matters and a way to hear the effect of moving it. Volume
 * without a test tone is a setting you have to go and find something else to
 * check, which is how a volume slider ends up being adjusted by trial against
 * whatever happens to be playing.
 */
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

/* --- the interface ---------------------------------------------------------
 *
 * A sidebar and a page, both components. Each page is built rather than drawn:
 * the pixel arithmetic that used to place every pill - and that had to be
 * repeated exactly in the click handler, which is how the Appearance row came
 * to be a column of magic numbers - is gone, and a control is hit because it
 * is where the layout put it.
 *
 * Switching pages rebuilds the tree. It could instead hide five of six
 * subtrees, but a control panel builds a page in microseconds and the
 * alternative is six trees permanently alive with stale text in them.
 */

static struct app g_app;
static struct ui_view* g_note_label;
static int g_rebuild;               /* a page change, applied after the walk */

static const char* page_name(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < PAGES) ? kPages[row] : "";
}

static void on_page(struct ui_view* v, void* user)
{
    (void)user;
    if (v->selected < 0 || v->selected >= PAGES || v->selected == g_page)
        return;
    g_page = v->selected;
    g_note[0] = '\0';
    /* Not here: this runs inside the walk over the tree that is about to be
     * freed. */
    g_rebuild = 1;
}

/* A row of a settings group: what it is on the left, what it says on the
 * right. Returns the row so a control can be hung on it. */
static struct ui_view* row(struct ui_view* parent, const char* key)
{
    struct ui_view* r = ui_box(parent, UI_STACK_H, 0, 8);
    ui_size(r, 0, 24);
    ui_grow(r, 0);
    ui_grow(ui_label(r, key), 0);
    ui_spacer(r);
    return r;
}

static void kv(struct ui_view* parent, const char* key, const char* value)
{
    ui_grow(ui_label(row(parent, key), value), 0);
}

/* --- general --------------------------------------------------------------- */

static void build_general(struct ui_view* page)
{
    char name[64] = "?", line[96], cwd[128] = "";
    username(getuid(), name);
    getcwd(cwd, sizeof(cwd));

    struct ui_view* g = ui_group(page, "Account", UI_STACK_V, 12, 2);
    ui_grow(g, 0);
    ui_size(g, 0, 3 * 24 + 28);
    kv(g, "User", name);
    snprintf(line, sizeof(line), "%u", getuid());
    kv(g, "User ID", line);
    kv(g, "Directory", cwd);

    struct ui_view* c = ui_group(page, "This Computer", UI_STACK_V, 12, 2);
    ui_grow(c, 0);
    ui_size(c, 0, 3 * 24 + 28);

    struct fb_info fb;
    if (fb_info(&fb) == 0)
        snprintf(line, sizeof(line), "%ux%u, %u bpp", fb.width, fb.height,
                 fb.bits_per_pixel);
    else
        snprintf(line, sizeof(line), "none");
    kv(c, "Display", line);

    struct mem_info m;
    if (mem_info(&m) == 0)
        snprintf(line, sizeof(line), "%llu of %llu KiB used",
                 (unsigned long long)(m.used / 1024),
                 (unsigned long long)(m.usable / 1024));
    else
        snprintf(line, sizeof(line), "unknown");
    kv(c, "Memory", line);

    struct proc_info procs[64];
    const int n = proc_list(procs, 64);
    snprintf(line, sizeof(line), "%d", n < 0 ? 0 : n);
    kv(c, "Tasks", line);
    ui_spacer(page);
}

/* --- appearance ------------------------------------------------------------ */

static const char* mode_name(void* user, int i)
{
    (void)user;
    return (i >= 0 && i < PRESETS) ? kPresets[i].name : "";
}

static const char* backdrop_name(void* user, int i)
{
    (void)user;
    return i == 0 ? "Opaque" : "Blurred";
}

static const char* size_name(void* user, int i)
{
    (void)user;
    return i == 0 ? "Normal" : "Large";
}

static const char* pattern_name(void* user, int i)
{
    (void)user;
    return (i >= 0 && i < WS_PATTERN_COUNT) ? kPatterns[i] : "";
}

static void on_mode(struct ui_view* v, void* user)
{
    (void)user;
    apply_preset(v->on);
    g_rebuild = 1;
}

static void on_backdrop(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0) return;
    g_ws->theme.blur = (uint32_t)(v->on != 0);
    theme_changed();
    ui_set_text(g_note_label, g_note);
}

static void on_text_size(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0) return;
    g_ws->theme.text_scale = v->on == 0 ? 1u : 2u;
    theme_changed();
    ui_set_text(g_note_label, g_note);
}

static void on_pattern(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0 || v->selected < 0 || v->selected >= WS_PATTERN_COUNT) return;
    g_ws->theme.pattern = (uint32_t)v->selected;
    theme_changed();
}

static void on_choose_paper(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    app_sheet_file(&g_app, PATH_WALLPAPERS);
}

static void on_clear_paper(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    set_wallpaper("");
    ui_set_text(g_note_label, g_note);
}

static void build_appearance(struct ui_view* page)
{
    struct ui_view* t = ui_group(page, "Theme", UI_STACK_V, 12, 4);
    ui_grow(t, 0);
    ui_size(t, 0, 2 * 30 + 28);

    struct ui_view* r = row(t, "Appearance");
    struct ui_view* seg = ui_segmented(r, mode_name, PRESETS, 0);
    seg->on = g_mode;
    ui_size(seg, 160, 24);
    ui_grow(seg, 0);
    ui_on(seg, on_mode, 0);

    r = row(t, "Window Backdrop");
    seg = ui_segmented(r, backdrop_name, 2, 0);
    seg->on = (g_ws != 0 && g_ws->theme.blur != 0);
    ui_size(seg, 160, 24);
    ui_grow(seg, 0);
    ui_on(seg, on_backdrop, 0);

    struct ui_view* d = ui_group(page, "Desktop", UI_STACK_V, 12, 4);
    ui_grow(d, 0);
    ui_size(d, 0, 3 * 30 + 28);

    r = row(d, "Text Size");
    seg = ui_segmented(r, size_name, 2, 0);
    seg->on = (g_ws != 0 && g_ws->theme.text_scale == 2);
    ui_size(seg, 160, 24);
    ui_grow(seg, 0);
    ui_on(seg, on_text_size, 0);

    r = row(d, "Backdrop Pattern");
    struct ui_view* pop = ui_popup(r, pattern_name, WS_PATTERN_COUNT, 0);
    pop->selected = g_ws != 0 ? (int)g_ws->theme.pattern : 0;
    ui_size(pop, 160, 24);
    ui_grow(pop, 0);
    ui_on(pop, on_pattern, 0);

    r = row(d, "Wallpaper");
    ui_grow(ui_size(ui_button(r, "Choose a Picture...", on_choose_paper, 0),
                    150, 24), 0);
    ui_grow(ui_size(ui_button(r, "Remove", on_clear_paper, 0), 84, 24), 0);

    ui_grow(ui_label(page,
                     "saved to ~/.leahrc and restored when settings next starts"),
            0);
    ui_spacer(page);
}

/* --- sound ----------------------------------------------------------------- */

static struct ui_view* g_vol_slider;
static struct ui_view* g_vol_text;
static struct ui_view* g_mute;

static void show_volume(void)
{
    char v[16];
    const int now = audio_volume();
    if (g_vol_slider != 0) g_vol_slider->value = now;
    if (g_vol_text != 0) {
        snprintf(v, sizeof(v), "%d%%", now);
        ui_set_text(g_vol_text, v);
    }
    if (g_mute != 0) ui_set_text(g_mute, now == 0 ? "Unmute" : "Mute");
}

static void on_volume(struct ui_view* v, void* user)
{
    (void)user;
    set_audio_volume(v->value);
    show_volume();
}

static void on_mute(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    const int now = audio_volume();
    if (now == 0) {
        set_audio_volume(g_vol_before_mute);
    } else {
        g_vol_before_mute = now;
        set_audio_volume(0);
    }
    show_volume();
}

static void on_tone(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    test_tone();
    ui_set_text(g_note_label, g_note);
}

static void build_sound(struct ui_view* page)
{
    g_vol_slider = g_vol_text = g_mute = 0;
    if (!g_audio.present) {
        ui_grow(ui_label(page, "no audio device found"), 0);
        ui_spacer(page);
        return;
    }

    struct ui_view* g = ui_group(page, "Output", UI_STACK_V, 12, 6);
    ui_grow(g, 0);
    ui_size(g, 0, 3 * 30 + 28);

    struct ui_view* r = ui_box(g, UI_STACK_H, 0, 10);
    ui_size(r, 0, 24);
    ui_grow(r, 0);
    ui_grow(ui_label(r, "Volume"), 0);
    g_vol_slider = ui_slider(r, audio_volume(), 100);
    ui_on(g_vol_slider, on_volume, 0);
    g_vol_text = ui_grow(ui_size(ui_label(r, "0%"), 44, 24), 0);

    r = ui_box(g, UI_STACK_H, 0, 10);
    ui_size(r, 0, 26);
    ui_grow(r, 0);
    g_mute = ui_grow(ui_size(ui_button(r, "Mute", on_mute, 0), 84, 24), 0);
    ui_grow(ui_size(ui_button(r, "Test Tone", on_tone, 0), 100, 24), 0);
    ui_spacer(r);

    kv(g, "Device", g_audio.name);

    char f[64];
    snprintf(f, sizeof(f), "%u Hz, %u channels, 16-bit", g_audio.rate,
             g_audio.channels);
    struct ui_view* d = ui_group(page, "Format", UI_STACK_V, 12, 2);
    ui_grow(d, 0);
    ui_size(d, 0, 24 + 28);
    kv(d, "Stream", f);

    ui_grow(ui_label(page,
                     "the volume is saved to ~/.leahrc and restored at login"),
            0);
    ui_spacer(page);
    show_volume();
}

/* --- network --------------------------------------------------------------- */

static void build_network(struct ui_view* page)
{
    struct netinfo ni;
    char line[64];
    if (netinfo(&ni) != 0) {
        ui_grow(ui_label(page, "no interface is configured"), 0);
        ui_spacer(page);
        return;
    }
    struct ui_view* g = ui_group(page, "Interface", UI_STACK_V, 12, 2);
    ui_grow(g, 0);
    ui_size(g, 0, 4 * 24 + 28);

    snprintf(line, sizeof(line), "%u.%u.%u.%u", (ni.ip >> 24) & 0xFF,
             (ni.ip >> 16) & 0xFF, (ni.ip >> 8) & 0xFF, ni.ip & 0xFF);
    kv(g, "Address", line);
    snprintf(line, sizeof(line), "%u.%u.%u.%u", (ni.netmask >> 24) & 0xFF,
             (ni.netmask >> 16) & 0xFF, (ni.netmask >> 8) & 0xFF,
             ni.netmask & 0xFF);
    kv(g, "Netmask", line);
    snprintf(line, sizeof(line), "%u.%u.%u.%u", (ni.gateway >> 24) & 0xFF,
             (ni.gateway >> 16) & 0xFF, (ni.gateway >> 8) & 0xFF,
             ni.gateway & 0xFF);
    kv(g, "Gateway", line);
    snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0],
             ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    kv(g, "Hardware", line);

    ui_grow(ui_label(page,
                     "the address is fixed at boot; there is no DHCP client yet"),
            0);
    ui_spacer(page);
}

/* --- users ----------------------------------------------------------------- */

static struct ui_view* g_name_field;
static struct ui_view* g_pass_field;

static void take_fields(void)
{
    if (g_name_field != 0)
        snprintf(g_uname, sizeof(g_uname), "%s", ui_text(g_name_field));
    if (g_pass_field != 0)
        snprintf(g_upass, sizeof(g_upass), "%s", ui_text(g_pass_field));
}

static void after_account(void)
{
    /* add_user and reset_password clear the password once it has been used;
     * the field has to be cleared with it, or the next press sends whatever is
     * still on screen. */
    if (g_pass_field != 0)
        ui_set_text(g_pass_field, g_upass);
    ui_set_text(g_note_label, g_note);
}

static void on_create(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    take_fields();
    add_user();
    after_account();
}

static void on_set_password(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    take_fields();
    reset_password();
    after_account();
}

static void on_private(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    take_fields();
    set_home_mode(0700);
    ui_set_text(g_note_label, g_note);
}

static void on_shared(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    take_fields();
    set_home_mode(0755);
    ui_set_text(g_note_label, g_note);
}

static void build_users(struct ui_view* page)
{
    struct ui_view* g = ui_group(page, "Account", UI_STACK_V, 12, 6);
    ui_grow(g, 0);
    ui_size(g, 0, 3 * 30 + 28);

    struct ui_view* r = ui_box(g, UI_STACK_H, 0, 10);
    ui_size(r, 0, 24);
    ui_grow(r, 0);
    ui_grow(ui_size(ui_label(r, "Name"), 76, 24), 0);
    g_name_field = ui_field(r, g_uname);

    r = ui_box(g, UI_STACK_H, 0, 10);
    ui_size(r, 0, 24);
    ui_grow(r, 0);
    ui_grow(ui_size(ui_label(r, "Password"), 76, 24), 0);
    /* A secure field, which is what this always wanted: the old one drew its
     * own dots because there was nothing that did. */
    g_pass_field = ui_secure(r);
    ui_set_text(g_pass_field, g_upass);

    r = ui_box(g, UI_STACK_H, 0, 10);
    ui_size(r, 0, 26);
    ui_grow(r, 0);
    ui_grow(ui_size(ui_label(r, ""), 76, 24), 0);
    ui_grow(ui_size(ui_button(r, "Create", on_create, 0), 96, 24), 0);
    ui_grow(ui_size(ui_button(r, "Set Password", on_set_password, 0), 120, 24), 0);
    ui_spacer(r);

    struct ui_view* p = ui_group(page, "Home Directory", UI_STACK_V, 12, 6);
    ui_grow(p, 0);
    ui_size(p, 0, 2 * 30 + 28);
    ui_grow(ui_label(p, "who may look inside it"), 0);
    r = ui_box(p, UI_STACK_H, 0, 10);
    ui_size(r, 0, 26);
    ui_grow(r, 0);
    ui_grow(ui_size(ui_button(r, "Private (0700)", on_private, 0), 130, 24), 0);
    ui_grow(ui_size(ui_button(r, "Readable by All (0755)", on_shared, 0),
                    180, 24), 0);
    ui_spacer(r);

    ui_grow(ui_label(page, getuid() == 0
                           ? "you are root: you may change any account"
                           : "only root may create accounts"), 0);
    ui_spacer(page);
}

/* --- about ----------------------------------------------------------------- */

static void build_about(struct ui_view* page)
{
    struct ui_view* g = ui_group(page, "leahOS", UI_STACK_V, 12, 2);
    ui_grow(g, 0);
    ui_size(g, 0, 7 * 24 + 28);
    kv(g, "Kind", "a UNIX-like system for x86-64");
    kv(g, "Built", "from scratch: no third-party bootloader, no libc");
    kv(g, "Kernel", "NASM and C++23, higher half at -2 GiB");
    kv(g, "Storage", "ext2/3/4 read and write, AHCI, USB");
    kv(g, "Network", "e1000, IPv4, ARP, ICMP, UDP, DNS, TCP");
    kv(g, "Desktop", "a window server in userland on shared memory");

    struct mem_info m;
    char line[64];
    if (mem_info(&m) == 0) {
        snprintf(line, sizeof(line), "%llu MiB usable",
                 (unsigned long long)(m.usable / (1024 * 1024)));
        kv(g, "Memory", line);
    } else {
        kv(g, "Memory", "unknown");
    }
    ui_grow(ui_label(page, "README.md is the long version"), 0);
    ui_spacer(page);
}

/* --- the window ------------------------------------------------------------ */

static void build(void)
{
    ui_reset();
    g_name_field = g_pass_field = 0;

    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    struct ui_view* side = ui_sidebar(root, page_name, PAGES, 0);
    side->selected = g_page;
    ui_size(side, SIDEBAR, 0);
    ui_grow(side, 0);
    ui_on(side, on_page, 0);

    struct ui_view* page = ui_box(root, UI_STACK_V, 14, 10);
    ui_grow(page, 1);
    ui_grow(ui_size(ui_label(page, kPages[g_page]), 0, 22), 0);

    switch (g_page) {
    case PAGE_GENERAL: build_general(page);    break;
    case PAGE_APPEAR:  build_appearance(page); break;
    case PAGE_SOUND:   build_sound(page);      break;
    case PAGE_NETWORK: build_network(page);    break;
    case PAGE_USERS:   build_users(page);      break;
    default:           build_about(page);      break;
    }

    g_note_label = ui_grow(ui_size(ui_label(page, g_note), 0, 18), 0);
    g_app.root = root;
}

static int on_event(struct app* a, const struct win_event* e)
{
    (void)e;
    if (!g_rebuild)
        return 0;
    g_rebuild = 0;
    build();
    app_relayout(a);
    return 1;
}

static void on_sheet(struct app* a, int result)
{
    if (result != 1)
        return;
    set_wallpaper(app_sheet_path(a));
    ui_set_text(g_note_label, g_note);
}

int main(int argc, char** argv)
{
    /* The desktop's own control block, which is public precisely so that a
     * client like this can find it. */
    const int cid = shm_open(WS_CONTROL_KEY, 0, 0);
    if (cid >= 0)
        g_ws = (struct ws_shared*)shm_map(cid);
    apply_saved_theme();
    apply_saved_audio();

    g_app.title = "Settings";
    g_app.width = 660; g_app.height = 400;
    g_app.min_width = 620; g_app.min_height = 360;
    g_app.sidebar = SIDEBAR;
    g_app.event = on_event;
    g_app.sheet_done = on_sheet;
    build();
    return app_run(&g_app, argc, argv);
}
