/* shell - the status bar, the dock, and the panels they open.
 *
 * Four windows from one process, all of them in the overlay layer: the bar
 * across the top, the dock at the bottom, the panel the bar's left button
 * drops down, and the stack of notification cards at the right. One process
 * because they are one thing - pressing the bar opens the panel, and a panel
 * in another program would need a protocol to be told about it - and separate
 * windows because they are separate rectangles with gaps between them, and a
 * single window spanning the gaps would swallow every click that fell in one.
 *
 * The bar has no background of its own. What is drawn is a button, a clock and
 * a button, over whatever the wallpaper happens to be; the strip between them
 * is transparent. That is why the bar window is only as tall as the buttons -
 * anything it covers, it covers whether it drew there or not.
 *
 * None of this takes the keyboard. See WS_FLAG_OVERLAY: chrome that stole the
 * keys every time it was clicked would make the window you were typing in stop
 * answering, which is worse than having no dock.
 */

#include <bundle.h>
#include <display.h>
#include <icon.h>
#include <paths.h>
#include <proc.h>
#include <shm.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>
#include <wproto.h>

/* --- the shape of it --------------------------------------------------------
 *
 * Sizes for a screen of this size rather than a scale of the drawing: the
 * mockup is a picture at some larger logical size, and copying its numbers
 * would give a bar too thin to press and a clock too small to read. */
#define BAR_H       34
#define BUTTON      26          /* the round buttons at either end */
#define BAR_PAD     8

#define DOCK_ICON   40
#define DOCK_PAD    10
#define DOCK_GAP    8           /* between icons */
#define DOCK_H      (DOCK_ICON + DOCK_PAD * 2)
#define DOCK_BOTTOM 10          /* the gap under it */
#define DOCK_MAX    8

#define PANEL_W     196
#define PANEL_ROW   24
#define PANEL_APPS  8           /* the most it will list */
#define POWER_D     34          /* the round power buttons */

#define NOTE_W      312
#define NOTE_PAD    10
#define NOTE_GAP    8
#define NOTE_SHOWN  3           /* the most on screen at once */
#define NOTE_LIFE   12000       /* how long one stays up, in ms */

/* The white the panels and buttons are made of, and the shadow under them. The
 * bar's own furniture is opaque white so it reads against any wallpaper; the
 * dock is translucent, because it sits over the desktop rather than over
 * whatever a window is showing. */
#define WHITE       0xFFFFFFFFu
#define DOCK_TINT   0xB8FFFFFFu
#define INK         0xFF18202Bu

static unsigned g_screen_w = 1024, g_screen_h = 768;

/* The window server's control block, which is public precisely so a client
 * like this can find it: the window table is how "what is running" is
 * answered, and the notification ring lives here too. */
static struct ws_shared* g_ws;

/* --- the bar ---------------------------------------------------------------- */

static int       g_bar = -1;
static uint32_t* g_bar_px;
static unsigned  g_bar_stride;
static char      g_clock[16] = "";
static int       g_panel_open;

/* --- the dock --------------------------------------------------------------- */

struct pinned {
    char name[64];
    char exec[192];
    const uint32_t* icon;
};

static struct pinned g_dock_app[DOCK_MAX];
static int       g_dock_n;
static int       g_dock = -1;
static uint32_t* g_dock_px;
static unsigned  g_dock_stride;
static int       g_dock_w;
static int       g_dock_x;
static int       g_dock_hot = -1;       /* which icon the pointer is over */

/* --- the panel -------------------------------------------------------------- */

struct running { char title[48]; int slot; };

static struct running g_running[PANEL_APPS];
static int       g_running_n;
static int       g_panel = -1;
static uint32_t* g_panel_px;
static unsigned  g_panel_stride;
static int       g_panel_h;
static int       g_panel_hot = -1;      /* the row under the pointer */
static int       g_power_hot = -1;

/* --- the notifications ------------------------------------------------------ */

struct shown_note { char from[WS_NOTE_FROM]; char text[WS_NOTE_TEXT];
                    unsigned long at; int lines; };

static struct shown_note g_note[NOTE_SHOWN];
static int       g_note_n;
static uint32_t  g_note_seen;           /* the last sequence taken from the ring */
/* Held open by the bell rather than fading on their own. A notification that
 * went past while you were looking elsewhere is the one you most want back. */
static int       g_note_pinned;
static int       g_notes = -1;
static uint32_t* g_notes_px;
static unsigned  g_notes_stride;
static int       g_notes_h;

/* Twelve or twenty-four hour, the same setting the Clock app reads. Re-read
 * rather than remembered, because Settings writes it while this is running. */
#include <prefs.h>

static int wants_24_hour(void)
{
    static int answer = 1;
    static unsigned long asked;
    const unsigned long now = uptime_ms();
    if (asked == 0 || now - asked > 3000) {
        asked = now;
        prefs_scope(PREFS_DESKTOP);
        prefs_load();
        answer = prefs_get_u32("clock.24hour", 1) != 0;
    }
    return answer;
}

/* The time as the bar shows it. Returns whether it changed, so the bar is
 * redrawn once a minute rather than once a tick. */
static int refresh_clock(void)
{
    char text[16];
    const time_t now = time(0);
    struct tm t;
    if (localtime_r(&now, &t) == 0) {
        snprintf(text, sizeof(text), "--:--");
    } else if (wants_24_hour()) {
        snprintf(text, sizeof(text), "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
        int h = t.tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(text, sizeof(text), "%d:%02d %s", h, t.tm_min,
                 t.tm_hour < 12 ? "AM" : "PM");
    }
    if (strcmp(text, g_clock) == 0)
        return 0;
    snprintf(g_clock, sizeof(g_clock), "%s", text);
    return 1;
}

/* A filled circle. The buttons at either end of the bar are round, and a
 * rounded rectangle as wide as it is tall is a circle - so this is the one
 * call rather than a second piece of geometry. */
static void circle(int x, int y, int d, uint32_t argb)
{
    wg_glass_fill(x, y, d, d, d / 2, argb);
}

/* Three dots, which is what the left button is. */
static void draw_dots(int cx, int cy)
{
    for (int i = -1; i <= 1; ++i)
        wg_glass_fill(cx + i * 6 - 1, cy - 1, 3, 3, 1, INK);
}

/* A bell, drawn rather than loaded: it is six rectangles and an icon file for
 * it would be a file to keep in step with the rest of the bar's colour. */
static void draw_bell(int cx, int cy)
{
    wg_glass_fill(cx - 5, cy - 4, 10, 7, 3, INK);       /* the body   */
    wg_glass_fill(cx - 7, cy + 2, 14, 2, 1, INK);       /* the rim    */
    wg_glass_fill(cx - 1, cy - 7, 2, 2, 1, INK);        /* the handle */
    wg_glass_fill(cx - 2, cy + 4, 4, 2, 1, INK);        /* the clapper */
}

static void paint_bar(void)
{
    if (g_bar_px == 0)
        return;
    wg_target_strided(g_bar_px, g_screen_w, BAR_H, g_bar_stride);
    wg_theme();

    /* Nothing behind it: the strip between the three things below is the
     * wallpaper, and a window's buffer is what the server composites. Alpha
     * zero is "not there at all", which is exactly right. */
    for (unsigned y = 0; y < BAR_H; ++y) {
        uint32_t* row = &g_bar_px[(unsigned long)y * g_bar_stride];
        for (unsigned x = 0; x < g_screen_w; ++x)
            row[x] = 0;
    }

    const int by = (BAR_H - BUTTON) / 2;
    circle(BAR_PAD, by, BUTTON, g_panel_open ? 0xFFE6EAF0u : WHITE);
    draw_dots(BAR_PAD + BUTTON / 2, by + BUTTON / 2);

    const int rx = (int)g_screen_w - BAR_PAD - BUTTON;
    circle(rx, by, BUTTON, g_note_pinned ? 0xFFE6EAF0u : WHITE);
    draw_bell(rx + BUTTON / 2, by + BUTTON / 2);

    /* The clock is white on the wallpaper rather than in a pill of its own,
     * which is what the drawing asks for and what makes the bar look like part
     * of the picture instead of a strip laid over it. */
    const int tw = wg_styled_width(g_clock, (int)strlen(g_clock), 15, 0);
    wg_styled(((int)g_screen_w - tw) / 2, (BAR_H - wg_styled_height(15)) / 2,
              g_clock, (int)strlen(g_clock), WHITE, 15, 0);
    win_present(g_bar);
}

/* What has a window, which is what "running" means to somebody looking at a
 * screen - not what has a process. The window table is the list; a program
 * with two windows appears once, because it is one application. */
static int scan_running(void)
{
    struct running found[PANEL_APPS];
    int n = 0;
    const int slots = ws_slot_count();
    for (int i = 0; i < slots && n < PANEL_APPS; ++i) {
        struct ws_window* w = ws_slot(i);
        if (w == 0 || w->state != WS_SLOT_LIVE)
            continue;
        if ((w->flags & (WS_FLAG_DESKTOP | WS_FLAG_OVERLAY |
                         WS_FLAG_SHEET | WS_FLAG_HIDDEN)) != 0)
            continue;
        if (w->title[0] == '\0')
            continue;
        int seen = 0;
        for (int k = 0; k < n; ++k)
            if (strcmp(found[k].title, w->title) == 0) { seen = 1; break; }
        if (seen)
            continue;
        snprintf(found[n].title, sizeof(found[n].title), "%s", w->title);
        found[n].slot = i;
        ++n;
    }

    /* Only rebuild when it has actually changed, so the panel is not redrawn
     * sixteen times a second while it is open. */
    if (n == g_running_n) {
        int same = 1;
        for (int i = 0; i < n; ++i)
            if (strcmp(found[i].title, g_running[i].title) != 0) { same = 0; break; }
        if (same)
            return 0;
    }
    for (int i = 0; i < n; ++i)
        g_running[i] = found[i];
    g_running_n = n;
    return 1;
}

/* A heading, the applications, a rule, a heading, and the row of buttons. */
static int height_for(int apps)
{
    return 12 + 18 + apps * PANEL_ROW + 12 + 18 + POWER_D + 16 + 10;
}

static int panel_height(void) { return height_for(g_running_n); }

/* As tall as it will ever need to be. The window's buffer is fixed once it is
 * made and the list grows as applications open, so it is made at the size of
 * the longest list it will show and draws only the part it is using - the rest
 * of the buffer stays transparent and nothing is there. Sizing it to the list
 * at startup is what cut the power buttons off: at that moment there were two
 * applications, and by the time anybody pressed the button there were four. */
static int panel_max(void) { return height_for(PANEL_APPS); }

static int panel_row_y(int i) { return 12 + 18 + i * PANEL_ROW; }

static int power_y(void) { return panel_height() - 10 - 16 - POWER_D; }

static int power_x(int i)
{
    const int span = POWER_D * 3 + 16 * 2;
    return (PANEL_W - span) / 2 + i * (POWER_D + 16);
}

/* A power symbol, a circling arrow and a moon: three glyphs small enough that
 * drawing them is shorter than three more files to install. */
static void draw_power_glyph(int which, int cx, int cy)
{
    if (which == 0) {                           /* off: a ring with a break */
        wg_glass_outline(cx - 6, cy - 6, 13, 13, 6, 2, INK);
        wg_glass_fill(cx - 3, cy - 8, 6, 5, 0, WHITE);
        wg_glass_fill(cx - 1, cy - 8, 2, 7, 1, INK);
    } else if (which == 1) {                    /* reboot: a ring and a nib */
        wg_glass_outline(cx - 6, cy - 6, 13, 13, 6, 2, INK);
        wg_glass_fill(cx + 1, cy - 8, 6, 4, 0, WHITE);
        wg_glass_fill(cx + 2, cy - 7, 5, 2, 1, INK);
        wg_glass_fill(cx + 5, cy - 8, 2, 5, 1, INK);
    } else {                                    /* sleep: a crescent */
        wg_glass_fill(cx - 6, cy - 6, 13, 13, 6, INK);
        wg_glass_fill(cx - 2, cy - 8, 12, 12, 6, WHITE);
    }
}

static void paint_panel(void)
{
    if (g_panel_px == 0 || g_panel < 0)
        return;
    wg_target_strided(g_panel_px, PANEL_W, (unsigned)g_panel_h,
                      g_panel_stride);
    wg_theme();
    for (int y = 0; y < g_panel_h; ++y) {
        uint32_t* row = &g_panel_px[(unsigned long)y * g_panel_stride];
        for (int x = 0; x < PANEL_W; ++x)
            row[x] = 0;
    }
    wg_glass_fill(0, 0, PANEL_W, panel_height(), 12, WHITE);

    wg_styled(12, 10, "Running Apps", 12, 0xFF6B7280u, 11, WG_STYLE_BOLD);
    for (int i = 0; i < g_running_n; ++i) {
        const int y = panel_row_y(i);
        if (i == g_panel_hot)
            wg_glass_fill(6, y - 2, PANEL_W - 12, PANEL_ROW - 2, 7,
                          0x1A2C6BEDu);
        wg_text_clipped(12, y + 2, g_running[i].title, INK, PANEL_W - 24);
    }

    const int py = power_y();
    wg_glass_fill(12, py - 20, PANEL_W - 24, 1, 0, 0x22000000u);
    {
        static const char* const kNames[3] = { "Shut Down", "Reboot", "Sleep" };
        wg_styled(12, py - 15, "Power Settings", 14, 0xFF6B7280u, 11,
                  WG_STYLE_BOLD);
        for (int i = 0; i < 3; ++i) {
            const int x = power_x(i);
            wg_glass_fill(x, py, POWER_D, POWER_D, POWER_D / 2,
                          i == g_power_hot ? 0xFFDCE3EDu : 0xFFF0F2F5u);
            draw_power_glyph(i, x + POWER_D / 2, py + POWER_D / 2);
            const int tw = wg_styled_width(kNames[i], (int)strlen(kNames[i]),
                                           10, 0);
            wg_styled(x + POWER_D / 2 - tw / 2, py + POWER_D + 3, kNames[i],
                      (int)strlen(kNames[i]), 0xFF6B7280u, 10, 0);
        }
    }
    win_present(g_panel);
}

/* --- the notifications ------------------------------------------------------
 *
 * Taken from the ring in the control block. A sequence is remembered rather
 * than a position, so a burst that overruns the ring while nothing was reading
 * shows the last few and quietly drops the rest - which is what should happen
 * to a notification nobody saw for a minute. */
static int note_height(const struct shown_note* n)
{
    return NOTE_PAD * 2 + 12 + n->lines * 15;
}

/* How many lines the message needs at this width. Measured rather than
 * guessed, because a card the wrong height either clips its own text or leaves
 * a band of white under it. */
static int wrap_lines(const char* text, int width)
{
    int lines = 1, start = 0, last_space = -1;
    const int n = (int)strlen(text);
    for (int i = 0; i < n; ++i) {
        if (text[i] == ' ')
            last_space = i;
        if (wg_styled_width(&text[start], i - start + 1, 12, 0) > width) {
            const int cut = last_space > start ? last_space : i;
            start = cut + 1;
            last_space = -1;
            ++lines;
        }
    }
    return lines;
}

/* One line of the message at a time, breaking on spaces. */
static void draw_wrapped(int x, int y, const char* text, int width)
{
    int start = 0, last_space = -1;
    const int n = (int)strlen(text);
    for (int i = 0; i < n; ++i) {
        if (text[i] == ' ')
            last_space = i;
        if (wg_styled_width(&text[start], i - start + 1, 12, 0) > width) {
            const int cut = last_space > start ? last_space : i;
            wg_styled(x, y, &text[start], cut - start, INK, 12, 0);
            y += 15;
            start = cut + 1;
            last_space = -1;
        }
    }
    if (start < n)
        wg_styled(x, y, &text[start], n - start, INK, 12, 0);
}

static void paint_notes(void)
{
    if (g_notes_px == 0 || g_notes < 0 || g_note_n == 0)
        return;
    wg_target_strided(g_notes_px, NOTE_W, (unsigned)g_notes_h,
                      g_notes_stride);
    wg_theme();
    for (int y = 0; y < g_notes_h; ++y) {
        uint32_t* row = &g_notes_px[(unsigned long)y * g_notes_stride];
        for (int x = 0; x < NOTE_W; ++x)
            row[x] = 0;
    }
    int y = 0;
    for (int i = 0; i < g_note_n; ++i) {
        const int h = note_height(&g_note[i]);
        wg_glass_fill(0, y, NOTE_W, h, 10, WHITE);
        char from[WS_NOTE_FROM + 8];
        snprintf(from, sizeof(from), "From %s", g_note[i].from);
        wg_styled(NOTE_PAD, y + NOTE_PAD, from, (int)strlen(from),
                  0xFF9AA3B0u, 10, WG_STYLE_BOLD);
        draw_wrapped(NOTE_PAD, y + NOTE_PAD + 12, g_note[i].text,
                     NOTE_W - NOTE_PAD * 2);
        y += h + NOTE_GAP;
    }
    win_present(g_notes);
}

/* Everything the ring still holds, newest last, whether or not it has been
 * shown before. What the bell is for: a card that faded while you were looking
 * at something else is the one worth being able to ask for again. */
static void recall_notes(void)
{
    struct ws_shared* b = g_ws;
    g_note_n = 0;
    if (b == 0)
        return;
    const uint32_t next = __atomic_load_n(&b->notes.next, __ATOMIC_ACQUIRE);
    const uint32_t from = next > NOTE_SHOWN ? next - NOTE_SHOWN : 0;
    for (uint32_t seq = from + 1; seq <= next; ++seq) {
        struct ws_note* r = &b->notes.ring[(seq - 1) % WS_NOTES_MAX];
        if (__atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) != seq)
            continue;
        struct shown_note* n = &g_note[g_note_n++];
        snprintf(n->from, sizeof(n->from), "%s", r->from);
        snprintf(n->text, sizeof(n->text), "%s", r->text);
        n->at = uptime_ms();
        n->lines = wrap_lines(n->text, NOTE_W - NOTE_PAD * 2);
    }
}

/* Take whatever is new, and drop whatever has been up long enough. Returns
 * whether the stack changed. */
static int poll_notes(void)
{
    struct ws_shared* b = g_ws;
    int changed = 0;
    if (b != 0) {
        const uint32_t next = __atomic_load_n(&b->notes.next, __ATOMIC_ACQUIRE);
        /* Anything older than the ring holds has been overwritten; start from
         * the oldest that still exists rather than reading rubbish. */
        uint32_t from = g_note_seen;
        if (next > WS_NOTES_MAX && from < next - WS_NOTES_MAX)
            from = next - WS_NOTES_MAX;
        for (uint32_t seq = from + 1; seq <= next; ++seq) {
            struct ws_note* r = &b->notes.ring[(seq - 1) % WS_NOTES_MAX];
            if (__atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) != seq)
                continue;               /* being written; catch it next pass */
            if (g_note_n == NOTE_SHOWN) {
                for (int i = 1; i < NOTE_SHOWN; ++i)
                    g_note[i - 1] = g_note[i];
                --g_note_n;
            }
            struct shown_note* n = &g_note[g_note_n++];
            snprintf(n->from, sizeof(n->from), "%s", r->from);
            snprintf(n->text, sizeof(n->text), "%s", r->text);
            n->at = uptime_ms();
            n->lines = wrap_lines(n->text, NOTE_W - NOTE_PAD * 2);
            changed = 1;
        }
        g_note_seen = next;
    }

    if (g_note_pinned)
        return changed;                 /* held open: nothing expires */

    const unsigned long now = uptime_ms();
    int keep = 0;
    for (int i = 0; i < g_note_n; ++i) {
        if (now - g_note[i].at < NOTE_LIFE)
            g_note[keep++] = g_note[i];
        else
            changed = 1;
    }
    g_note_n = keep;
    return changed;
}

/* --- the dock --------------------------------------------------------------- */

/* What to pin, by name and in this order.
 *
 * Named rather than "whatever the applications directory lists first", which
 * is what the disk happened to write and put the four test programs at the
 * front. A person choosing their own would be a preference and a sheet to edit
 * it; until there is one, this is the answer to "which six would somebody
 * want", and an application that is not installed is simply skipped. */
static const char* const kPinned[] = {
    "Files", "Terminal", "Edit", "Web", "Calculator", "Settings"
};

static void dock_scan(void)
{
    const int wanted = (int)(sizeof(kPinned) / sizeof(kPinned[0]));
    for (int i = 0; i < wanted && g_dock_n < DOCK_MAX; ++i) {
        struct bundle b;
        if (bundle_find(kPinned[i], &b) != 0)
            continue;
        const char* path = b.path;
        struct pinned* p = &g_dock_app[g_dock_n++];
        snprintf(p->name, sizeof(p->name), "%s", b.name);
        bundle_exec(&b, p->exec, sizeof(p->exec));
        /* The picture inside the bundle, by its own name for it. icon_by_path
         * wants the file and not the directory holding it - every bundle has
         * an Icon.png, so a bundle that names something else is the case worth
         * following rather than the common one. */
        char picture[256];
        snprintf(picture, sizeof(picture), "%s/%s", path,
                 b.icon[0] != '\0' ? b.icon : "Icon.png");
        p->icon = icon_by_path(picture);
        if (p->icon == 0)
            p->icon = icon_by_name("binary");
    }
    g_dock_w = g_dock_n > 0
             ? DOCK_PAD * 2 + g_dock_n * DOCK_ICON + (g_dock_n - 1) * DOCK_GAP
             : 0;
    g_dock_x = ((int)g_screen_w - g_dock_w) / 2;
}

static int dock_icon_x(int i)
{
    return DOCK_PAD + i * (DOCK_ICON + DOCK_GAP);
}

static void paint_dock(void)
{
    if (g_dock_px == 0 || g_dock_n == 0)
        return;
    wg_target_strided(g_dock_px, (unsigned)g_dock_w, DOCK_H, g_dock_stride);
    wg_theme();
    for (unsigned y = 0; y < DOCK_H; ++y) {
        uint32_t* row = &g_dock_px[(unsigned long)y * g_dock_stride];
        for (int x = 0; x < g_dock_w; ++x)
            row[x] = 0;
    }
    wg_glass_fill(0, 0, g_dock_w, DOCK_H, 16, DOCK_TINT);

    for (int i = 0; i < g_dock_n; ++i) {
        const int x = dock_icon_x(i);
        /* The one under the pointer is lifted by a highlight behind it rather
         * than by growing: growing would move the ones beside it, and a row of
         * targets that shift as the pointer crosses them is a row that is hard
         * to hit. */
        if (i == g_dock_hot)
            wg_glass_fill(x - 4, DOCK_PAD - 4, DOCK_ICON + 8, DOCK_ICON + 8,
                          10, 0x33FFFFFFu);
        if (g_dock_app[i].icon != 0)
            wg_icon_scaled(x, DOCK_PAD, g_dock_app[i].icon,
                           ICON_SIZE, ICON_SIZE, DOCK_ICON, DOCK_ICON);
    }
    win_present(g_dock);
}

/* Start something, detached: the shell is not waiting for it, and a dock that
 * blocked until the application exited would be a dock that opened one thing
 * ever. */
static void launch(const char* exec)
{
    const int pid = fork();
    if (pid != 0)
        return;
    char* argv[] = { (char*)exec, 0 };
    execve(exec, argv, environ);
    exit(127);
}

/* --- putting it up ---------------------------------------------------------- */

static int make_window(int x, int y, unsigned w, unsigned h, const char* title,
                       uint32_t** px, unsigned* stride)
{
    const int id = win_create(x, y, w, h, title);
    if (id < 0)
        return -1;
    win_set_overlay(id);
    win_set_alpha(id);
    win_set_client_title(id);           /* no title bar: this is not a window */
    *px = win_map(id);
    *stride = win_stride(id);
    return id;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    const int cid = shm_open(WS_CONTROL_KEY, 0, 0);
    if (cid >= 0)
        g_ws = (struct ws_shared*)shm_map(cid);

    struct fb_info fb;
    if (fb_info(&fb) == 0 && fb.width > 0) {
        g_screen_w = fb.width;
        g_screen_h = fb.height;
    }

    dock_scan();

    g_bar = make_window(0, 0, g_screen_w, BAR_H, "Status Bar",
                        &g_bar_px, &g_bar_stride);
    if (g_bar < 0) {
        printf("shell: no window server\n");
        return 1;
    }
    if (g_dock_n > 0)
        g_dock = make_window(g_dock_x,
                             (int)g_screen_h - DOCK_H - DOCK_BOTTOM,
                             (unsigned)g_dock_w, DOCK_H, "Dock",
                             &g_dock_px, &g_dock_stride);

    /* What is left for everything else. Claimed here rather than by the window
     * server, because the server has no idea what furniture is running - this
     * is the process that put it there. */
    win_set_work_area(0, BAR_H, (int)g_screen_w,
                      (int)g_screen_h - BAR_H -
                      (g_dock >= 0 ? DOCK_H + DOCK_BOTTOM : 0));

    /* The panel and the cards are made at their largest and hidden. Their
     * height changes with what is in them - a list of two applications is
     * shorter than one of six - and a window cannot be resized from inside
     * without replacing its buffer, so they are as tall as they will ever need
     * and draw only the part they are using. */
    scan_running();
    g_panel_h = panel_max();
    g_panel = make_window(BAR_PAD, BAR_H + 4, PANEL_W, (unsigned)g_panel_h,
                          "Panel", &g_panel_px, &g_panel_stride);
    if (g_panel >= 0)
        win_hide(g_panel);

    /* Room for the most cards there can be, each as tall as the longest
     * message will wrap to. Shown at whatever the stack actually needs, for
     * the same reason as the panel. */
    g_notes_h = NOTE_SHOWN * (NOTE_PAD * 2 + 12 + 4 * 15 + NOTE_GAP);
    g_notes = make_window((int)g_screen_w - NOTE_W - BAR_PAD, BAR_H + 4,
                          NOTE_W, (unsigned)g_notes_h,
                          "Notifications", &g_notes_px, &g_notes_stride);
    if (g_notes >= 0)
        win_hide(g_notes);

    refresh_clock();
    paint_bar();
    paint_dock();

    for (;;) {
        struct win_event e;
        int redraw_bar = 0, redraw_dock = 0, redraw_notes = 0;

        int redraw_panel = 0;

        while (win_poll(g_bar, &e)) {
            if (e.type != WIN_EVENT_MOUSE_DOWN)
                continue;
            const int by = (BAR_H - BUTTON) / 2;
            const int rx = (int)g_screen_w - BAR_PAD - BUTTON;
            if (e.x >= rx && e.x < rx + BUTTON &&
                e.y >= by && e.y < by + BUTTON) {
                /* The bell. Open, it holds the recent messages on screen;
                 * shut, it puts them away and lets new ones fade as before. */
                g_note_pinned = !g_note_pinned;
                if (g_note_pinned)
                    recall_notes();
                else
                    g_note_n = 0;
                redraw_notes = 1;
                redraw_bar = 1;
                continue;
            }
            if (e.x >= BAR_PAD && e.x < BAR_PAD + BUTTON &&
                e.y >= by && e.y < by + BUTTON) {
                g_panel_open = !g_panel_open;
                if (g_panel >= 0) {
                    if (g_panel_open) {
                        scan_running();
                        /* Only as tall as the list needs. The buffer is made
                         * for the longest list there can be; showing less of
                         * it is what stops the unused remainder taking clicks
                         * meant for whatever is underneath. */
                        win_set_size(g_panel, PANEL_W,
                                     (unsigned)panel_height());
                        redraw_panel = 1;
                        win_show(g_panel);
                        win_raise(g_panel);
                    } else {
                        win_hide(g_panel);
                    }
                }
                redraw_bar = 1;
            }
        }

        if (g_panel >= 0 && g_panel_open) {
            while (win_poll(g_panel, &e)) {
                if (e.type != WIN_EVENT_MOUSE_DOWN &&
                    e.type != WIN_EVENT_MOUSE_MOVE)
                    continue;
                int row = -1, power = -1;
                for (int i = 0; i < g_running_n; ++i)
                    if (e.y >= panel_row_y(i) &&
                        e.y < panel_row_y(i) + PANEL_ROW)
                        row = i;
                const int py = power_y();
                if (e.y >= py && e.y < py + POWER_D)
                    for (int i = 0; i < 3; ++i)
                        if (e.x >= power_x(i) && e.x < power_x(i) + POWER_D)
                            power = i;
                if (row != g_panel_hot || power != g_power_hot) {
                    g_panel_hot = row;
                    g_power_hot = power;
                    redraw_panel = 1;
                }
                if (e.type != WIN_EVENT_MOUSE_DOWN)
                    continue;
                if (row >= 0) {
                    win_raise(g_running[row].slot);
                    g_panel_open = 0;
                    win_hide(g_panel);
                    redraw_bar = 1;
                } else if (power == 0) {
                    power_off();
                } else if (power == 1) {
                    power_reboot();
                } else if (power == 2) {
                    if (g_ws != 0)
                        __atomic_store_n(&g_ws->input.blank_now, 1,
                                         __ATOMIC_RELEASE);
                    g_panel_open = 0;
                    win_hide(g_panel);
                    redraw_bar = 1;
                }
            }
            if (scan_running()) {
                win_set_size(g_panel, PANEL_W, (unsigned)panel_height());
                redraw_panel = 1;
            }
        }

        if (g_dock >= 0) {
            while (win_poll(g_dock, &e)) {
                if (e.type == WIN_EVENT_MOUSE_MOVE ||
                    e.type == WIN_EVENT_MOUSE_DOWN) {
                    int over = -1;
                    for (int i = 0; i < g_dock_n; ++i) {
                        const int x = dock_icon_x(i);
                        if (e.x >= x && e.x < x + DOCK_ICON &&
                            e.y >= DOCK_PAD && e.y < DOCK_PAD + DOCK_ICON)
                            over = i;
                    }
                    if (over != g_dock_hot) {
                        g_dock_hot = over;
                        redraw_dock = 1;
                    }
                    if (e.type == WIN_EVENT_MOUSE_DOWN && over >= 0)
                        launch(g_dock_app[over].exec);
                }
            }
        }

        if (poll_notes())
            redraw_notes = 1;
        if (redraw_notes) {
            if (g_notes >= 0) {
                if (g_note_n > 0) {
                    int h = 0;
                    for (int i = 0; i < g_note_n; ++i)
                        h += note_height(&g_note[i]) + NOTE_GAP;
                    win_set_size(g_notes, NOTE_W,
                                 (unsigned)(h > NOTE_GAP ? h - NOTE_GAP : 1));
                    win_show(g_notes);
                    win_raise(g_notes);
                } else {
                    win_hide(g_notes);
                }
            }
        }

        if (refresh_clock())
            redraw_bar = 1;
        if (redraw_bar)   paint_bar();
        if (redraw_dock)  paint_dock();
        if (redraw_panel) paint_panel();
        if (redraw_notes) paint_notes();

        msleep(60);
    }
}
