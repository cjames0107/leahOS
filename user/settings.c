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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <shm.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>
#include <wproto.h>

#define PAGE_GENERAL  0
#define PAGE_APPEAR   1
#define PAGE_SCREEN   2
#define PAGE_SOUND    3
#define PAGE_MOUSE    4
#define PAGE_STORAGE  5
#define PAGE_NETWORK  6
#define PAGE_DATETIME 7
#define PAGE_USERS    8
#define PAGE_SHELL    9
#define PAGE_TERMINAL 10
#define PAGE_ABOUT    11
#define PAGES         12

#define SIDEBAR 148
#define ROW_H   22

static int g_page = PAGE_GENERAL;

/* The sidebar, as one list with headings in it.
 *
 * Twelve pages in a flat column is a list that has to be read rather than
 * scanned, which is the point at which every control panel grows sections. The
 * headings are rows like any other so that there is one array and one set of
 * indices; ui_sidebar_headings is what says which of them are labels. A
 * heading names no page, which is what -1 means. */
struct entry { const char* label; int page; };

static const struct entry kRows[] = {
    { "Desktop",     -1 },
    { "General",     PAGE_GENERAL },
    { "Appearance",  PAGE_APPEAR },
    { "Screen",      PAGE_SCREEN },
    { "Hardware",    -1 },
    { "Sound",       PAGE_SOUND },
    { "Mouse",       PAGE_MOUSE },
    { "Storage",     PAGE_STORAGE },
    { "System",      -1 },
    { "Network",     PAGE_NETWORK },
    { "Date & Time", PAGE_DATETIME },
    { "Users",       PAGE_USERS },
    { "About",       PAGE_ABOUT },
    { "UNIX",        -1 },
    { "Shell",       PAGE_SHELL },
    { "Terminal",    PAGE_TERMINAL },
};
#define ROWS ((int)(sizeof(kRows) / sizeof(kRows[0])))

static const char* row_label(void* user, int i)
{
    (void)user;
    return (i >= 0 && i < ROWS) ? kRows[i].label : "";
}

static int row_is_heading(void* user, int i)
{
    (void)user;
    return (i >= 0 && i < ROWS) ? kRows[i].page < 0 : 0;
}

/* Which row shows the page now open, so the sidebar can be built with the
 * right one already chosen. */
static int row_of_page(int page)
{
    for (int i = 0; i < ROWS; ++i)
        if (kRows[i].page == page)
            return i;
    return 1;
}

static const char* page_title(int page)
{
    for (int i = 0; i < ROWS; ++i)
        if (kRows[i].page == page)
            return kRows[i].label;
    return "";
}

/* The output device, asked about once, and the volume to come back to when
 * mute is switched off. */
static struct audio_info g_audio;
static int g_vol_before_mute = 70;
static char g_note[128] = "";

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
    uint32_t desktop, face, title, title_text, cursor;
    uint32_t selection, body, text;
};
static const struct preset kPresets[] = {
    { "Light", 0x8894A8, 0xF2F4F7, 0xF2F4F7, 0x18202B,
      0xFFFFFF, 0x2C6BED, 0xFFFFFF, 0x18202B },
    /* Dark, and dark all the way through: a window face that is dark with ink
     * that is still black is not a dark mode, it is an unreadable light one. */
    { "Dark",  0x1B2028, 0x272C34, 0x272C34, 0xE8ECF2,
      0xE8ECF2, 0x3E7BF0, 0x1E232A, 0xE8ECF2 },
};
#define PRESETS (int)(sizeof(kPresets) / sizeof(kPresets[0]))
#define MODE_LIGHT 0
#define MODE_DARK  1

/* Written through to the user's file as well as to the running desktop, so a
 * choice survives the session that made it. */
/* The desktop's preferences rather than this application's.
 *
 * app_run picks a scope named after the window, which is where this program's
 * own geometry belongs - and it does so after main has run, so by the time
 * anything here writes a setting the scope has moved. Every write about the
 * desktop has to say which file it means and put the scope back after.
 *
 * Without that the theme was written into ~/.config/Settings and read back at
 * the next launch from ~/.config/desktop, so nothing chosen here had ever
 * survived a restart - the page said it would, which is worse than not
 * offering it. */
static void desktop_prefs(void)
{
    prefs_scope(PREFS_DESKTOP);
    prefs_load();
}

static void own_prefs(void)
{
    prefs_scope("Settings");
    prefs_load();
}

static void theme_changed(void)
{
    if (g_ws == 0)
        return;
    __atomic_add_fetch(&g_ws->theme.generation, 1, __ATOMIC_RELEASE);

    desktop_prefs();
    prefs_set_u32("theme.desktop", g_ws->theme.desktop);
    prefs_set_u32("theme.face", g_ws->theme.face);
    prefs_set_u32("theme.title", g_ws->theme.title_active);
    prefs_set_u32("theme.cursor", g_ws->theme.cursor);
    prefs_set_u32("theme.selection", g_ws->theme.selection);
    prefs_set_u32("theme.body", g_ws->theme.body);
    prefs_set_u32("theme.text", g_ws->theme.text);
    prefs_set_u32("theme.scale", g_ws->theme.text_scale);
    prefs_set_u32("theme.blur", g_ws->theme.blur);
    prefs_set_u32("theme.mode", (unsigned)g_mode);
    prefs_set_str("theme.wallpaper", (const char*)g_ws->theme.wallpaper);
    if (prefs_save() != 0)
        snprintf(g_note, sizeof(g_note), "changed, but could not be saved");
    own_prefs();
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
    /* The desktop's, not this application's. What the windows look like is not
     * a setting belonging to the program that happens to change it, and an
     * application that reads it - a future one that wants the accent colour -
     * should not have to know Settings wrote it. */
    prefs_scope(PREFS_DESKTOP);
    prefs_load();
    g_ws->theme.desktop      = prefs_get_u32("theme.desktop", g_ws->theme.desktop);
    g_ws->theme.face         = prefs_get_u32("theme.face", g_ws->theme.face);
    g_ws->theme.title_active = prefs_get_u32("theme.title", g_ws->theme.title_active);
    g_ws->theme.cursor       = prefs_get_u32("theme.cursor", g_ws->theme.cursor);
    g_ws->theme.selection  = prefs_get_u32("theme.selection", g_ws->theme.selection);
    g_ws->theme.body       = prefs_get_u32("theme.body", g_ws->theme.body);
    g_ws->theme.text       = prefs_get_u32("theme.text", g_ws->theme.text);
    g_ws->theme.text_scale = prefs_get_u32("theme.scale", 1);
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



/* --- the pointer, the wheel and the screen ---------------------------------
 *
 * These live in the same control block as the theme, for the same reason: they
 * are the desktop's, not this application's, and the processes that act on
 * them - the compositor and the mouse driver - find them there. Every one of
 * them was a constant compiled into one of those two before.
 *
 * Saved and restored like the theme, because the control block is built fresh
 * every time the server starts and has no idea whose desktop it is. */
static void input_changed(void)
{
    if (g_ws == 0)
        return;
    desktop_prefs();
    prefs_set_u32("input.natural_scroll", g_ws->input.natural_scroll);
    prefs_set_u32("input.scroll_lines", g_ws->input.scroll_lines);
    prefs_set_u32("input.pointer_speed", g_ws->input.pointer_speed);
    prefs_set_u32("input.blank_ms", g_ws->input.blank_ms);
    prefs_set_u32("theme.shadows", g_ws->theme.shadows);
    if (prefs_save() != 0)
        snprintf(g_note, sizeof(g_note), "changed, but could not be saved");
    own_prefs();
}

static void apply_saved_input(void)
{
    if (g_ws == 0)
        return;
    g_ws->input.natural_scroll = prefs_get_u32("input.natural_scroll", 0);
    g_ws->input.scroll_lines   = prefs_get_u32("input.scroll_lines", 3);
    g_ws->input.pointer_speed  = prefs_get_u32("input.pointer_speed", 100);
    g_ws->input.blank_ms       = prefs_get_u32("input.blank_ms", 0);
    g_ws->theme.shadows        = prefs_get_u32("theme.shadows", 1);
}

static void apply_preset(int i)
{
    g_mode = i;
    if (g_ws == 0 || i < 0 || i >= PRESETS)
        return;
    const struct preset* p = &kPresets[i];
    g_ws->theme.desktop      = p->desktop;
    g_ws->theme.face         = p->face;
    g_ws->theme.title_active = p->title;
    g_ws->theme.title_text   = p->title_text;
    g_ws->theme.cursor       = p->cursor;
    g_ws->theme.selection    = p->selection;
    g_ws->theme.body         = p->body;
    g_ws->theme.text         = p->text;
    /* The wallpaper is not part of the preset. It used to be cleared here, so
     * choosing Dark threw away the picture and the only way back was to pick
     * it again from the sheet - a light switch that also emptied the room.
     * What a preset decides is the colours; the picture has its own two
     * buttons a few rows down. */
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

static void on_page(struct ui_view* v, void* user)
{
    (void)user;
    if (v->selected < 0 || v->selected >= ROWS)
        return;
    const int page = kRows[v->selected].page;
    if (page < 0 || page == g_page)
        return;
    g_page = page;
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
    ui_fit(g);
    kv(g, "User", name);
    snprintf(line, sizeof(line), "%u", getuid());
    kv(g, "User ID", line);
    kv(g, "Directory", cwd);

    struct ui_view* c = ui_group(page, "This Computer", UI_STACK_V, 12, 2);
    ui_fit(c);

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

static void on_shadows(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0) return;
    g_ws->theme.shadows = (uint32_t)(v->on != 0);
    __atomic_add_fetch(&g_ws->theme.generation, 1, __ATOMIC_RELEASE);
    input_changed();
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
    ui_fit(t);

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

    /* Both of these buy their looks with the compositor's time, which on a
     * machine with no acceleration is the whole budget - so they are together,
     * and both can be turned off. */
    r = row(t, "Window Shadows");
    struct ui_view* sh = ui_toggle(r, "", g_ws != 0 && g_ws->theme.shadows != 0);
    ui_size(sh, 52, 24);
    ui_grow(sh, 0);
    ui_on(sh, on_shadows, 0);

    struct ui_view* d = ui_group(page, "Desktop", UI_STACK_V, 12, 4);
    ui_fit(d);

    r = row(d, "Text Size");
    seg = ui_segmented(r, size_name, 2, 0);
    seg->on = (g_ws != 0 && g_ws->theme.text_scale == 2);
    ui_size(seg, 160, 24);
    ui_grow(seg, 0);
    ui_on(seg, on_text_size, 0);

    r = row(d, "Wallpaper");
    ui_grow(ui_size(ui_button(r, "Choose a Picture...", on_choose_paper, 0),
                    150, 24), 0);
    ui_grow(ui_size(ui_button(r, "Remove", on_clear_paper, 0), 84, 24), 0);

    ui_grow(ui_label(page,
                     "saved to ~/.config/desktop and restored at the next login"),
            0);
    ui_spacer(page);
}

/* --- screen ---------------------------------------------------------------
 *
 * The one thing on this page did not exist at all: a machine left alone kept
 * the same picture on the framebuffer indefinitely. The compositor blanks it
 * now, because it is the process that knows when the last thing happened and
 * the only one allowed to write to the screen. */

static const unsigned kBlank[] = { 0, 60000, 300000, 900000 };
#define BLANKS ((int)(sizeof(kBlank) / sizeof(kBlank[0])))

static const char* blank_name(void* user, int i)
{
    (void)user;
    static const char* const kNames[BLANKS] = {
        "Never", "1 min", "5 min", "15 min"
    };
    return (i >= 0 && i < BLANKS) ? kNames[i] : "";
}

static void on_blank(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0 || v->on < 0 || v->on >= BLANKS)
        return;
    g_ws->input.blank_ms = kBlank[v->on];
    input_changed();
    ui_set_text(g_note_label, g_note);
}

static void build_screen(struct ui_view* page)
{
    struct ui_view* g = ui_group(page, "Turn the Screen Off", UI_STACK_V, 12, 4);
    ui_fit(g);

    struct ui_view* r = row(g, "After");
    struct ui_view* seg = ui_segmented(r, blank_name, BLANKS, 0);
    int at = 0;
    for (int i = 0; i < BLANKS; ++i)
        if (g_ws != 0 && g_ws->input.blank_ms == kBlank[i])
            at = i;
    seg->on = at;
    ui_size(seg, 240, 24);
    ui_grow(seg, 0);
    ui_on(seg, on_blank, 0);

    ui_grow(ui_label(page, "a key or a movement of the mouse brings it back"), 0);
    ui_spacer(page);
}

/* --- mouse ----------------------------------------------------------------- */

static struct ui_view* g_speed_text;
static struct ui_view* g_lines_text;

static void show_pointer(void)
{
    char t[24];
    if (g_speed_text != 0 && g_ws != 0) {
        snprintf(t, sizeof(t), "%u%%", g_ws->input.pointer_speed);
        ui_set_text(g_speed_text, t);
    }
    if (g_lines_text != 0 && g_ws != 0) {
        const unsigned n = g_ws->input.scroll_lines;
        snprintf(t, sizeof(t), "%u line%s", n, n == 1 ? "" : "s");
        ui_set_text(g_lines_text, t);
    }
}

static void on_speed(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0)
        return;
    /* The slider runs 0 to 100 and the speed 25 to 400, because a pointer at a
     * quarter speed is slow and one at four times it is the fastest that is
     * still controllable. */
    g_ws->input.pointer_speed = 25u + (unsigned)v->value * 375u / 100u;
    input_changed();
    show_pointer();
}

static void on_lines(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0)
        return;
    g_ws->input.scroll_lines = (unsigned)(v->value + 1);
    input_changed();
    show_pointer();
}

static void on_natural(struct ui_view* v, void* user)
{
    (void)user;
    if (g_ws == 0)
        return;
    g_ws->input.natural_scroll = (unsigned)(v->on != 0);
    input_changed();
    ui_set_text(g_note_label, g_note);
}

static void build_mouse(struct ui_view* page)
{
    struct ui_view* g = ui_group(page, "Pointer", UI_STACK_V, 12, 4);
    ui_fit(g);

    struct ui_view* r = row(g, "Tracking Speed");
    const unsigned pc = g_ws != 0 ? g_ws->input.pointer_speed : 100;
    int at = (int)((pc > 25 ? pc - 25 : 0) * 100 / 375);
    if (at > 100) at = 100;
    struct ui_view* sl = ui_slider(r, at, 100);
    ui_size(sl, 180, WG_SLIDER_H);
    ui_grow(sl, 0);
    ui_on(sl, on_speed, 0);
    g_speed_text = ui_grow(ui_size(ui_label(r, ""), 54, 0), 0);

    struct ui_view* w = ui_group(page, "Wheel", UI_STACK_V, 12, 4);
    ui_fit(w);

    r = row(w, "Scroll by");
    const unsigned lines = g_ws != 0 ? g_ws->input.scroll_lines : 3;
    sl = ui_slider(r, (int)(lines > 0 ? lines - 1 : 2), 9);
    ui_size(sl, 180, WG_SLIDER_H);
    ui_grow(sl, 0);
    ui_on(sl, on_lines, 0);
    g_lines_text = ui_grow(ui_size(ui_label(r, ""), 54, 0), 0);

    r = row(w, "Natural Scrolling");
    struct ui_view* t = ui_toggle(r, "", g_ws != 0 &&
                                  g_ws->input.natural_scroll != 0);
    ui_size(t, 52, 24);
    ui_grow(t, 0);
    ui_on(t, on_natural, 0);

    ui_grow(ui_label(page, "natural scrolling moves the content with the wheel,"
                           " not against it"), 0);
    show_pointer();
    ui_spacer(page);
}

/* --- storage ---------------------------------------------------------------
 *
 * What is mounted and how full it is. Read-only, because there is nothing here
 * to set - but "how much room is left" is the question a control panel is
 * asked most often, and answering it needed df and a terminal. */

static void human(unsigned long long bytes, char* out, unsigned long max)
{
    if (bytes >= (1ull << 30))
        snprintf(out, max, "%llu.%llu GB", bytes >> 30,
                 ((bytes >> 20) % 1024) * 10 / 1024);
    else if (bytes >= (1ull << 20))
        snprintf(out, max, "%llu MB", bytes >> 20);
    else
        snprintf(out, max, "%llu KB", bytes >> 10);
}

static void build_storage(struct ui_view* page)
{
    FILE* in = fopen("/proc/mounts", "r");
    if (in == 0) {
        ui_grow(ui_label(page, "/proc/mounts is not there"), 0);
        ui_spacer(page);
        return;
    }
    char line[256];
    int shown = 0;
    while (fgets(line, sizeof(line), in) != 0) {
        char what[64], at[64], kind[32], how[16];
        if (sscanf(line, "%63s %63s %31s %15s", what, at, kind, how) != 4)
            continue;

        struct statfs fs;
        if (statfs(at, &fs) != 0 || fs.f_blocks == 0)
            continue;                   /* nothing to measure: procfs and such */

        const unsigned long long total =
            (unsigned long long)fs.f_blocks * fs.f_bsize;
        const unsigned long long free_b =
            (unsigned long long)fs.f_bfree * fs.f_bsize;
        const unsigned long long used = total - free_b;

        char title[96], sizes[96], a[24], b[24];
        snprintf(title, sizeof(title), "%s on %s", what, at);
        struct ui_view* g = ui_group(page, title, UI_STACK_V, 12, 4);
        ui_fit(g);

        human(used, a, sizeof(a));
        human(total, b, sizeof(b));
        snprintf(sizes, sizeof(sizes), "%s of %s used (%s)", a, b, kind);
        kv(g, "Capacity", sizes);

        /* The bar spans the group rather than sitting at the end of a row:
         * it is the same fact as the line above it, drawn, and a capacity bar
         * that stops a third of the way across reads as a third of a disk. */
        struct ui_view* bar = ui_progress(g, (int)(used / 1024),
                                          (int)(total / 1024));
        ui_size(bar, 0, 10);
        ui_grow(bar, 1);
        ++shown;
    }
    fclose(in);
    if (shown == 0)
        ui_grow(ui_label(page, "nothing is mounted that can be measured"), 0);
    ui_spacer(page);
}

/* --- date and time ---------------------------------------------------------
 *
 * The offset is a signed number of minutes in /etc/timezone, which libc has
 * always read and nothing has ever written. Not a zone name: naming a zone
 * means carrying the table that says what the zone did in 1987. */

static struct ui_view* g_now_label;
static struct ui_view* g_tz_stepper;
static char g_now_text[64];
static int  g_tz_minutes;

/* The offset written out, in the control that sets it. */
static void show_tz(void)
{
    if (g_tz_stepper == 0)
        return;
    char t[24];
    const int m = g_tz_minutes < 0 ? -g_tz_minutes : g_tz_minutes;
    snprintf(t, sizeof(t), "UTC%s%02d:%02d",
             g_tz_minutes < 0 ? "-" : "+", m / 60, m % 60);
    ui_set_text(g_tz_stepper, t);
}

static void write_timezone(void)
{
    char text[16];
    const int m = g_tz_minutes;
    snprintf(text, sizeof(text), "%s%d\n", m < 0 ? "-" : "+", m < 0 ? -m : m);
    const int fd = open("/etc/timezone", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(g_note, sizeof(g_note), "/etc/timezone is not writable");
        return;
    }
    write(fd, text, strlen(text));
    close(fd);
    snprintf(g_note, sizeof(g_note), "the zone is now UTC%s%02d:%02d",
             m < 0 ? "-" : "+", (m < 0 ? -m : m) / 60, (m < 0 ? -m : m) % 60);
}

static int clock_24(void)
{
    desktop_prefs();
    const int on = prefs_get_u32("clock.24hour", 1) != 0;
    own_prefs();
    return on;
}

static void show_now(void)
{
    const time_t now = time(0);
    struct tm t;
    if (localtime_r(&now, &t) == 0) {
        snprintf(g_now_text, sizeof(g_now_text), "unknown");
    } else if (clock_24()) {
        snprintf(g_now_text, sizeof(g_now_text), "%02d:%02d:%02d",
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        int h = t.tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(g_now_text, sizeof(g_now_text), "%d:%02d:%02d %s",
                 h, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "am" : "pm");
    }
    if (g_now_label != 0)
        ui_set_text(g_now_label, g_now_text);
}

static void on_tz(struct ui_view* v, void* user)
{
    (void)user;
    /* The stepper counts quarter hours from -12:00, which is every offset any
     * zone actually uses and none of the ones in between. */
    g_tz_minutes = (v->value * 15) - 12 * 60;
    show_tz();
    write_timezone();
    show_now();
    ui_set_text(g_note_label, g_note);
}

static void on_clock_format(struct ui_view* v, void* user)
{
    (void)user;
    desktop_prefs();
    prefs_set_u32("clock.24hour", (unsigned)(v->on == 0));
    prefs_save();
    own_prefs();
    show_now();
    ui_set_text(g_note_label, "the clock picks this up within a few seconds");
}

static const char* hour_name(void* user, int i)
{
    (void)user;
    return i == 0 ? "24-hour" : "12-hour";
}

static void build_datetime(struct ui_view* page)
{
    char line[64];

    struct ui_view* c = ui_group(page, "Clock", UI_STACK_V, 12, 4);
    ui_fit(c);
    struct ui_view* r = row(c, "Time");
    show_now();
    g_now_label = ui_grow(ui_label(r, g_now_text), 0);

    r = row(c, "Format");
    struct ui_view* seg = ui_segmented(r, hour_name, 2, 0);
    seg->on = clock_24() ? 0 : 1;
    ui_size(seg, 160, 24);
    ui_grow(seg, 0);
    ui_on(seg, on_clock_format, 0);

    struct ui_view* z = ui_group(page, "Time Zone", UI_STACK_V, 12, 4);
    ui_fit(z);

    g_tz_minutes = (int)(timezone_offset() / 60);
    r = row(z, "Offset from UTC");
    /* -12:00 to +14:00 in quarter hours: 105 steps, which covers every offset
     * any zone actually uses and none of the ones in between. */
    g_tz_stepper = ui_stepper(r, (g_tz_minutes + 12 * 60) / 15, 104);
    ui_size(g_tz_stepper, 150, 24);
    ui_grow(g_tz_stepper, 0);
    ui_on(g_tz_stepper, on_tz, 0);
    show_tz();
    (void)line;

    ui_grow(ui_label(page, "written to /etc/timezone; every program reads it"), 0);
    ui_spacer(page);
}

/* --- the shell -------------------------------------------------------------
 *
 * These are the environment, and the environment's home on a UNIX is a file
 * the shell reads at startup, not a preferences database. So this page edits
 * ~/.profile - which sh now sources for an interactive shell, and which did
 * not exist before, because there was nowhere to put a PATH: login compiled
 * one in and nothing could change it afterwards.
 *
 * The two lines this writes are replaced in place and everything else in the
 * file is kept, for the same reason prefs keeps keys it does not know: a
 * person's own additions are not this program's to discard.
 */

static struct ui_view* g_path_field;
static struct ui_view* g_shell_field;

static void profile_path(char* out, unsigned long max)
{
    const char* home = getenv("HOME");
    snprintf(out, max, "%s/.profile", home != 0 && home[0] != '\0' ? home : "/root");
}

/* The value of `export NAME=` in the profile, or what the environment says,
 * or the fallback - in that order, because the file is what will be in force
 * next time and the environment is only what is in force now. */
static void profile_get(const char* name, char* out, unsigned long max,
                        const char* fallback)
{
    const char* env = getenv(name);
    snprintf(out, max, "%s", env != 0 && env[0] != '\0' ? env : fallback);

    char path[256];
    profile_path(path, sizeof(path));
    FILE* in = fopen(path, "r");
    if (in == 0)
        return;
    char line[512], want[64];
    snprintf(want, sizeof(want), "export %s=", name);
    const unsigned long n = strlen(want);
    while (fgets(line, sizeof(line), in) != 0) {
        if (strncmp(line, want, n) != 0)
            continue;
        unsigned long len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        snprintf(out, max, "%s", &line[n]);
    }
    fclose(in);
}

/* Rewrite the file with these two lines set, keeping every other line. */
static int profile_put(const char* path_value, const char* shell_value)
{
    char path[256];
    profile_path(path, sizeof(path));

    /* Read it whole first: the file is being replaced, so it cannot be read
     * and written at the same time. */
    char kept[4096];
    unsigned long keep_n = 0;
    FILE* in = fopen(path, "r");
    if (in != 0) {
        char line[512];
        while (fgets(line, sizeof(line), in) != 0) {
            if (strncmp(line, "export PATH=", 12) == 0 ||
                strncmp(line, "export SHELL=", 13) == 0)
                continue;               /* these two are ours to rewrite */
            const unsigned long len = strlen(line);
            if (keep_n + len + 1 >= sizeof(kept))
                break;
            memcpy(&kept[keep_n], line, len);
            keep_n += len;
        }
        fclose(in);
    }
    kept[keep_n] = '\0';

    FILE* out = fopen(path, "w");
    if (out == 0)
        return -1;
    fputs("# Read by sh at the start of every interactive shell.\n", out);
    if (keep_n > 0)
        fputs(kept, out);
    fprintf(out, "export PATH=%s\n", path_value);
    fprintf(out, "export SHELL=%s\n", shell_value);
    fclose(out);
    return 0;
}

static void on_shell_save(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    const char* p = g_path_field != 0 ? g_path_field->text : "";
    const char* sh = g_shell_field != 0 ? g_shell_field->text : "";
    if (p[0] == '\0' || sh[0] == '\0') {
        snprintf(g_note, sizeof(g_note), "neither of these may be empty");
        ui_set_text(g_note_label, g_note);
        return;
    }
    struct stat st;
    if (stat(sh, &st) != 0) {
        snprintf(g_note, sizeof(g_note), "%s is not there", sh);
        ui_set_text(g_note_label, g_note);
        return;
    }
    if (profile_put(p, sh) != 0)
        snprintf(g_note, sizeof(g_note), "~/.profile could not be written");
    else
        snprintf(g_note, sizeof(g_note),
                 "saved - every shell started from now on will use it");
    ui_set_text(g_note_label, g_note);
}

static void build_shell(struct ui_view* page)
{
    char value[512];

    struct ui_view* g = ui_group(page, "Environment", UI_STACK_V, 12, 4);
    ui_fit(g);

    struct ui_view* r = row(g, "PATH");
    profile_get("PATH", value, sizeof(value),
                "/usr/local/bin:/bin:/usr/bin");
    g_path_field = ui_grow(ui_size(ui_field(r, value), 340, 24), 0);

    r = row(g, "Shell");
    profile_get("SHELL", value, sizeof(value), "/bin/sh");
    g_shell_field = ui_grow(ui_size(ui_field(r, value), 340, 24), 0);

    r = row(g, "");
    ui_grow(ui_size(ui_button(r, "Save", on_shell_save, 0), 90, 24), 0);

    struct ui_view* w = ui_group(page, "Where This Goes", UI_STACK_V, 12, 2);
    ui_fit(w);
    char path[256];
    profile_path(path, sizeof(path));
    kv(w, "File", path);
    kv(w, "Read by", "sh, at the start of every interactive shell");

    ui_grow(ui_label(page, "a terminal opened after saving picks both up"), 0);
    ui_spacer(page);
}

/* --- terminal --------------------------------------------------------------
 *
 * Written into the Terminal's own preferences rather than the desktop's,
 * because they are one application's. Its window opens at the size named here
 * and keeps this many lines of history. */

/* The steppers themselves carry the value, rather than a label beside them:
 * the number a stepper counts is an index here - eighty columns is the
 * fifteenth step - and showing both put "80" and "15" on the same row. */
static struct ui_view* g_cols_step;
static struct ui_view* g_rows_step;
static struct ui_view* g_back_step;
static unsigned g_t_cols, g_t_rows, g_t_back;

static void term_save(void)
{
    prefs_scope("Terminal");
    prefs_load();
    prefs_set_u32("columns", g_t_cols);
    prefs_set_u32("rows", g_t_rows);
    prefs_set_u32("scrollback", g_t_back);
    const int failed = prefs_save() != 0;
    own_prefs();
    snprintf(g_note, sizeof(g_note), failed
             ? "could not be saved"
             : "the next terminal window opens at this size");
    ui_set_text(g_note_label, g_note);
}

static void show_term(void)
{
    char t[24];
    if (g_cols_step != 0) {
        snprintf(t, sizeof(t), "%u", g_t_cols);
        ui_set_text(g_cols_step, t);
    }
    if (g_rows_step != 0) {
        snprintf(t, sizeof(t), "%u", g_t_rows);
        ui_set_text(g_rows_step, t);
    }
    if (g_back_step != 0) {
        snprintf(t, sizeof(t), "%u lines", g_t_back);
        ui_set_text(g_back_step, t);
    }
}

static void on_cols(struct ui_view* v, void* user)
{
    (void)user;
    g_t_cols = 20u + (unsigned)v->value * 4u;   /* 20 to 200, four at a time */
    show_term();
    term_save();
}

static void on_rows(struct ui_view* v, void* user)
{
    (void)user;
    g_t_rows = 4u + (unsigned)v->value * 2u;    /* 4 to 100, two at a time */
    show_term();
    term_save();
}

static void on_back(struct ui_view* v, void* user)
{
    (void)user;
    g_t_back = 128u + (unsigned)v->value * 128u;
    show_term();
    term_save();
}

static void build_terminal(struct ui_view* page)
{
    prefs_scope("Terminal");
    prefs_load();
    g_t_cols = prefs_get_u32("columns", 80);
    g_t_rows = prefs_get_u32("rows", 24);
    g_t_back = prefs_get_u32("scrollback", 1024);
    own_prefs();

    struct ui_view* g = ui_group(page, "New Windows", UI_STACK_V, 12, 4);
    ui_fit(g);

    struct ui_view* r = row(g, "Columns");
    g_cols_step = ui_grow(ui_size(ui_stepper(r, (int)((g_t_cols - 20) / 4), 45),
                                  126, 24), 0);
    ui_on(g_cols_step, on_cols, 0);

    r = row(g, "Rows");
    g_rows_step = ui_grow(ui_size(ui_stepper(r, (int)((g_t_rows - 4) / 2), 48),
                                  126, 24), 0);
    ui_on(g_rows_step, on_rows, 0);

    struct ui_view* h = ui_group(page, "History", UI_STACK_V, 12, 4);
    ui_fit(h);

    r = row(h, "Scrollback");
    g_back_step = ui_grow(ui_size(ui_stepper(r, (int)(g_t_back / 128) - 1, 7),
                                  150, 24), 0);
    ui_on(g_back_step, on_back, 0);

    show_term();
    ui_grow(ui_label(page, "1024 lines is the most the ring can hold"), 0);
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
    ui_fit(g);

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
    ui_fit(d);
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
    ui_fit(g);

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
    ui_fit(g);

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
    ui_fit(p);
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
    ui_fit(g);
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

/* How tall the page is, for the scroll view that holds it. A box knows this
 * about itself; it just has to be asked at the moment the width is settled. */
static int measure_page(struct ui_view* v, int width, void* user)
{
    (void)width; (void)user;
    return ui_natural_h(v);
}

static void build(void)
{
    ui_reset();
    g_name_field = g_pass_field = 0;

    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    struct ui_view* side = ui_sidebar(root, row_label, ROWS, 0);
    ui_sidebar_headings(side, row_is_heading);
    side->selected = row_of_page(g_page);
    ui_size(side, SIDEBAR, 0);
    ui_grow(side, 0);
    ui_on(side, on_page, 0);

    /* The page scrolls. Storage is as tall as the machine has filesystems and
     * Users grows with the account list, so a page that is taller than the
     * window is not an unusual case to be designed around afterwards. */
    struct ui_view* pane = ui_grow(ui_scroll(root), 1);
    struct ui_view* page = ui_box(pane, UI_STACK_V, 14, 10);
    ui_measure(page, measure_page);
    ui_grow(ui_size(ui_label(page, page_title(g_page)), 0, 22), 0);

    switch (g_page) {
    case PAGE_GENERAL:  build_general(page);    break;
    case PAGE_APPEAR:   build_appearance(page); break;
    case PAGE_SCREEN:   build_screen(page);     break;
    case PAGE_SOUND:    build_sound(page);      break;
    case PAGE_MOUSE:    build_mouse(page);      break;
    case PAGE_STORAGE:  build_storage(page);    break;
    case PAGE_NETWORK:  build_network(page);    break;
    case PAGE_DATETIME: build_datetime(page);   break;
    case PAGE_USERS:    build_users(page);      break;
    case PAGE_SHELL:    build_shell(page);      break;
    case PAGE_TERMINAL: build_terminal(page);   break;
    default:            build_about(page);      break;
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
    apply_saved_theme();        /* leaves the desktop scope loaded */
    apply_saved_input();
    apply_saved_audio();

    g_app.title = "Settings";
    /* Tall enough for the sidebar's sixteen rows without scrolling it, and
     * wide enough for a PATH to be read in the field that holds it. */
    g_app.width = 720; g_app.height = 460;
    g_app.min_width = 660; g_app.min_height = 380;
    g_app.sidebar = SIDEBAR;
    g_app.event = on_event;
    g_app.sheet_done = on_sheet;
    build();
    return app_run(&g_app, argc, argv);
}
