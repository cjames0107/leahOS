/* netutil - the questions you ask when the network is not working.
 *
 * Four of them, which is what the panes are: what am I, can I reach that, what
 * is that name, and who is on this wire. They exist as commands already -
 * ifconfig, ping, nslookup, arp - and the reason to gather them is that
 * diagnosis is never one of them. It is "the name does not resolve, so is the
 * resolver reachable, and do I even have an address" - three commands and the
 * memory of what the last one said, which is exactly what a window is good at
 * and a terminal is not.
 *
 * Nothing here runs in the background. A ping is sent when asked for and the
 * answer is waited for, because netd already bounds how long that wait can be
 * and a spinner over a two-second operation is decoration.
 */

#include <dialog.h>
#include <net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define SIDE_W 150
#define ROW_H  26
#define LINES  14

static uint32_t* g_px;
static unsigned  g_w = 620, g_h = 400;

enum { PANE_INFO, PANE_PING, PANE_LOOKUP, PANE_ARP, PANE_COUNT };
static const char* const kPanes[PANE_COUNT] = {
    "Interface", "Ping", "Lookup", "Neighbours"
};
static int g_pane;

/* One text field, shared: only one pane has an input at a time, and giving
 * each its own would mean three ways to type the same kind of thing. */
static char g_input[64] = "10.0.2.2";
static int  g_focus;

/* The output, as lines. Kept rather than printed so that a result stays on
 * screen while the next question is typed - which is the whole point of not
 * being a terminal. */
static char g_out[LINES][96];
static int  g_lines;

static void say(const char* text)
{
    if (g_lines >= LINES) {
        for (int i = 1; i < LINES; ++i)
            memcpy(g_out[i - 1], g_out[i], sizeof(g_out[0]));
        --g_lines;
    }
    snprintf(g_out[g_lines++], sizeof(g_out[0]), "%s", text);
}

static void clear_out(void) { g_lines = 0; }

static void ip_text(uint32_t ip, char* out, unsigned max)
{
    snprintf(out, max, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

/* --- the four questions --------------------------------------------------- */

static void do_info(void)
{
    clear_out();
    struct netinfo n;
    if (netinfo(&n) != 0) {
        say("no network: netd is not answering");
        return;
    }
    char line[96], a[24];
    ip_text(n.ip, a, sizeof(a));
    snprintf(line, sizeof(line), "address    %s", a);       say(line);
    ip_text(n.netmask, a, sizeof(a));
    snprintf(line, sizeof(line), "netmask    %s", a);       say(line);
    ip_text(n.gateway, a, sizeof(a));
    snprintf(line, sizeof(line), "gateway    %s", a);       say(line);
    snprintf(line, sizeof(line),
             "hardware   %02x:%02x:%02x:%02x:%02x:%02x",
             n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    say(line);
}

static void do_ping(void)
{
    clear_out();
    uint32_t ip = 0;
    /* A name is accepted as well as an address, because "ping the gateway" and
     * "ping a host I know by name" are the same question to the person asking
     * and only different to the resolver. */
    if (parse_ip(g_input, &ip) != 0 && resolve(g_input, &ip) != 0) {
        say("that is neither an address nor a name I can resolve");
        return;
    }
    char line[96], a[24];
    ip_text(ip, a, sizeof(a));
    snprintf(line, sizeof(line), "pinging %s", a);
    say(line);

    int had = 0;
    for (uint16_t seq = 1; seq <= 4; ++seq) {
        uint8_t ttl = 0;
        if (ping(ip, seq, &ttl) == 0) {
            snprintf(line, sizeof(line), "  reply %u, ttl %u", seq, ttl);
            ++had;
        } else {
            snprintf(line, sizeof(line), "  no reply %u", seq);
        }
        say(line);
    }
    snprintf(line, sizeof(line), "%d of 4 answered", had);
    say(line);
}

static void do_lookup(void)
{
    clear_out();
    uint32_t ip = 0;
    char line[96], a[24];
    if (resolve(g_input, &ip) == 0) {
        ip_text(ip, a, sizeof(a));
        snprintf(line, sizeof(line), "%s is %s", g_input, a);
        say(line);
    } else {
        snprintf(line, sizeof(line), "%s did not resolve", g_input);
        say(line);
        say("the resolver may be unreachable - try pinging it");
    }
}

/* Who answers on this wire.
 *
 * There is no table to read: arp() asks for one address at a time, so this
 * sweeps the addresses worth asking about rather than inventing a listing that
 * the stack does not keep. The gateway and this host are always included
 * because they are the two that matter when nothing works. */
static void do_arp(void)
{
    clear_out();
    struct netinfo n;
    if (netinfo(&n) != 0) {
        say("no network: netd is not answering");
        return;
    }
    char line[96], a[24];
    const uint32_t base = n.ip & n.netmask;
    int found = 0;
    for (uint32_t host = 1; host < 16; ++host) {
        const uint32_t ip = base | host;
        uint8_t mac[6];
        if (arp(ip, mac) != 0)
            continue;
        ip_text(ip, a, sizeof(a));
        snprintf(line, sizeof(line), "%-16s %02x:%02x:%02x:%02x:%02x:%02x",
                 a, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        say(line);
        ++found;
        if (found >= LINES - 1)
            break;
    }
    if (found == 0)
        say("nobody answered on the first fifteen addresses");
}

static void ask(void)
{
    if (g_pane == PANE_INFO)        do_info();
    else if (g_pane == PANE_PING)   do_ping();
    else if (g_pane == PANE_LOOKUP) do_lookup();
    else                            do_arp();
}

/* --- drawing -------------------------------------------------------------- */

static int field_y(void) { return 52; }
static int field_x(void) { return SIDE_W + 16; }
static int field_w(void) { return (int)g_w - SIDE_W - 32 - 84; }
static int go_x(void)    { return SIDE_W + 16 + field_w() + 8; }

static int has_field(void) { return g_pane == PANE_PING || g_pane == PANE_LOOKUP; }

static void draw(void)
{
    wg_theme();
    wg_glass_clear();

    wg_sidebar(0, 0, SIDE_W, (int)g_h);
    wg_text(14, 12, "Network", WG_DIM);
    for (int i = 0; i < PANE_COUNT; ++i) {
        const int y = 34 + i * ROW_H;
        if (i == g_pane)
            wg_fill(6, y - 4, SIDE_W - 12, ROW_H, wg_sel_colour());
        wg_text(14, y, kPanes[i], wg_ink_colour());
    }

    const int cx = SIDE_W + 16;
    wg_text(cx, 16, kPanes[g_pane], wg_ink_colour());

    if (has_field()) {
        wg_field(cx, field_y(), field_w(), 24, g_input, g_focus);
        wg_button(go_x(), field_y(), 76, 24,
                  g_pane == PANE_PING ? "Ping" : "Look up", 0);
    } else {
        wg_button(cx, field_y(), 76, 24, "Refresh", 0);
    }

    const int oy = field_y() + 38;
    wg_container(cx, oy, (int)g_w - cx - 16, (int)g_h - oy - 12, 8);
    for (int i = 0; i < g_lines; ++i) {
        const int y = oy + 8 + i * WG_GLYPH_H;
        if (y + WG_GLYPH_H > (int)g_h - 16)
            break;
        wg_text_clipped(cx + 8, y, g_out[i], wg_ink_colour(),
                        (int)g_w - cx - 32);
    }
    if (g_lines == 0)
        wg_text(cx + 8, oy + 8, "nothing asked yet", WG_DIM);
}

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 200;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 130;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Network Utility");
    if (id < 0) {
        printf("netutil: no window server\n");
        return 1;
    }
    win_set_alpha(id);
    win_set_sidebar(id, SIDE_W);
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 520, 320);
    wg_target(g_px, g_w, g_h);

    do_info();
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
                if (e.x < SIDE_W) {
                    const int hit = (e.y - 30) / ROW_H;
                    if (hit >= 0 && hit < PANE_COUNT) {
                        g_pane = hit;
                        clear_out();
                        g_focus = 0;
                        /* The two panes with nothing to type answer at once;
                         * the two with a field wait to be told what to ask
                         * about. */
                        if (!has_field())
                            ask();
                    }
                } else if (e.y >= field_y() && e.y < field_y() + 24) {
                    if (has_field() && e.x >= field_x() &&
                        e.x < field_x() + field_w())
                        g_focus = 1;
                    else if (e.x >= (has_field() ? go_x() : field_x()) &&
                             e.x < (has_field() ? go_x() : field_x()) + 76) {
                        g_focus = 0;
                        ask();
                    }
                }
            } else if (e.type == WIN_EVENT_KEY) {
                if (g_focus && has_field()) {
                    const unsigned n = (unsigned)strlen(g_input);
                    if (e.key == '\b' && n > 0)
                        g_input[n - 1] = '\0';
                    else if (e.key == '\n') { g_focus = 0; ask(); }
                    else if (e.key >= ' ' && e.key < 127 &&
                             n + 1 < sizeof(g_input)) {
                        g_input[n] = (char)e.key;
                        g_input[n + 1] = '\0';
                    }
                } else if (e.key == '\n') {
                    ask();
                } else if (e.key == WIN_KEY_DOWN && g_pane + 1 < PANE_COUNT) {
                    ++g_pane; clear_out(); if (!has_field()) ask();
                } else if (e.key == WIN_KEY_UP && g_pane > 0) {
                    --g_pane; clear_out(); if (!has_field()) ask();
                } else {
                    continue;
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
