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

#include <app.h>
#include <ui.h>
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

enum { PANE_INFO, PANE_PING, PANE_LOOKUP, PANE_ARP, PANE_COUNT };
static const char* const kPanes[PANE_COUNT] = {
    "Interface", "Ping", "Lookup", "Neighbours"
};
static int g_pane;

/* One text field, shared: only one pane has an input at a time, and giving
 * each its own would mean three ways to type the same kind of thing. */
static char g_input[64] = "10.0.2.2";

/* The output, as lines. Kept rather than printed so that a result stays on
 * screen while the next question is typed - which is the whole point of not
 * being a terminal. */
static char g_out_lines[LINES][96];
static int  g_lines;

static void say(const char* text)
{
    if (g_lines >= LINES) {
        for (int i = 1; i < LINES; ++i)
            memcpy(g_out_lines[i - 1], g_out_lines[i], sizeof(g_out_lines[0]));
        --g_lines;
    }
    snprintf(g_out_lines[g_lines++], sizeof(g_out_lines[0]), "%s", text);
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

static int has_field(void)
{
    return g_pane == PANE_PING || g_pane == PANE_LOOKUP;
}

static void ask(void)
{
    if (g_pane == PANE_INFO)        do_info();
    else if (g_pane == PANE_PING)   do_ping();
    else if (g_pane == PANE_LOOKUP) do_lookup();
    else                            do_arp();
}

/* --- the interface ---------------------------------------------------------
 *
 * A sidebar of questions and a pane that answers one. The output is a list
 * rather than lines drawn by hand, so it scrolls when an answer is longer than
 * the window - which the drawn version could not do.
 */

static struct app g_app;
static struct ui_view* g_side;
static struct ui_view* g_title;
static struct ui_view* g_field;
static struct ui_view* g_go;
static struct ui_view* g_out;

static const char* out_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < g_lines) ? g_out_lines[row] : "";
}

static const char* pane_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < PANE_COUNT) ? kPanes[row] : "";
}

/* What the pane needs, after whatever just happened. */
static void sync_pane(void)
{
    ui_set_text(g_title, kPanes[g_pane]);
    if (has_field()) {
        g_field->flags &= ~UI_HIDDEN;
        ui_set_text(g_go, g_pane == PANE_PING ? "Ping" : "Look up");
    } else {
        g_field->flags |= UI_HIDDEN;
        ui_set_text(g_go, "Refresh");
    }
    g_out->rows = g_lines;
    app_relayout(&g_app);
}

static void on_pane(struct ui_view* v, void* user)
{
    (void)user;
    if (v->selected < 0 || v->selected >= PANE_COUNT)
        return;
    g_pane = v->selected;
    clear_out();
    /* The two panes with nothing to type answer at once; the two with a field
     * wait to be told what to ask about. */
    if (!has_field())
        ask();
    sync_pane();
}

static void on_go(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    snprintf(g_input, sizeof(g_input), "%s", ui_text(g_field));
    ask();
    sync_pane();
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    g_side = ui_sidebar(root, pane_row, PANE_COUNT, 0);
    ui_on(g_side, on_pane, 0);
    ui_size(g_side, 150, 0);
    g_side->selected = 0;

    struct ui_view* pane = ui_box(root, UI_STACK_V, 16, 10);
    g_title = ui_label(pane, kPanes[0]);
    ui_grow(g_title, 0);

    struct ui_view* row = ui_box(pane, UI_STACK_H, 0, 8);
    ui_size(row, 0, 26);
    ui_grow(row, 0);
    g_field = ui_field(row, g_input);
    ui_on(g_field, on_go, 0);       /* Return in the field asks */
    g_go = ui_button(row, "Refresh", on_go, 0);
    ui_grow(g_go, 0);
    ui_spacer(row);

    g_out = ui_list(pane, out_row, 0, 0);

    g_app.title = "Network Utility";
    g_app.width = 620; g_app.height = 400;
    g_app.min_width = 520; g_app.min_height = 320;
    g_app.sidebar = 150;
    g_app.root = root;

    do_info();
    /* sync_pane needs the app laid out, which app_run does first. */
    g_out->rows = g_lines;
    g_field->flags |= UI_HIDDEN;
    return app_run(&g_app, argc, argv);
}
