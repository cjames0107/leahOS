/* web - a browser, for the part of the web that is documents.
 *
 * It fetches over HTTP, parses the tags that carry meaning, lays the result
 * out as wrapped text with headings and links, and follows those links. It has
 * tabs, bookmarks and history, and the last two survive a reboot because a
 * bookmark that does not is a note written on a hand.
 *
 * WHAT IT IS NOT
 *
 * There is no JavaScript, no CSS, no images in the page, and no tables. Saying
 * that plainly matters more than it might: a browser that renders a modern
 * site badly is worse than one that says it cannot, because the first leaves
 * you wondering whether the page is broken or the browser is. Most of the web
 * will therefore come out as a long column of text, and text is exactly what
 * this can do well.
 *
 * The parser is a tokeniser rather than a tree builder. A real one would build
 * a DOM, and a DOM only earns its cost when something needs to query or mutate
 * it - which is scripting, which is not here. For laying out a document once,
 * a flow of styled runs is the whole job.
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

#define TABS_MAX     8
#define URL_MAX      256
#define PAGE_MAX     (192 * 1024)
#define RUNS_MAX     4096
#define LINKS_MAX    512
#define HISTORY_MAX  200
#define MARKS_MAX    64
#define SIDE_W       190

#define TAB_H        28
#define BAR_H        34
#define HEAD_H       (TAB_H + BAR_H)

/* A run of text with one style. The whole document is a list of these, which
 * is what a page is once scripting and boxes are off the table. */
struct run {
    unsigned start, len;        /* into the tab's text arena */
    unsigned char heading;      /* 0 body, 1..3 h1..h3 */
    unsigned char bold;
    unsigned char br;           /* a break follows this run */
    unsigned char item;         /* a list item's bullet precedes it */
    int link;                   /* index into the tab's links, or -1 */
};

struct link { char href[URL_MAX]; int x, y, w, h; };

struct tab {
    char url[URL_MAX];
    char title[80];
    char status[96];
    char* text;                 /* the flattened text of the page */
    unsigned text_len;
    struct run* runs;
    int runs_n;
    struct link* links;
    int links_n;
    int scroll;
    /* Where this tab has been, so Back is per tab as it should be: going back
     * in one tab has nothing to do with what another was showing. */
    char back[16][URL_MAX];
    int  back_n;
    char forward[16][URL_MAX];
    int  forward_n;
    int  used;
};

static struct tab g_tab[TABS_MAX];
static int g_cur;

/* The arenas live outside the tabs so that eight tabs do not need eight of
 * everything at once: only the loaded ones hold their storage, and a tab that
 * has never been used costs a struct. */
static char       g_arena[TABS_MAX][PAGE_MAX / 4];
static struct run g_runs[TABS_MAX][RUNS_MAX];
static struct link g_links[TABS_MAX][LINKS_MAX];
static char       g_raw[PAGE_MAX];


enum { SIDE_NONE, SIDE_MARKS, SIDE_HISTORY };
static int g_side = SIDE_NONE;

static char g_history[HISTORY_MAX][URL_MAX];
static int  g_history_n;
static char g_marks[MARKS_MAX][URL_MAX];
static char g_mark_title[MARKS_MAX][80];
static int  g_marks_n;

#define MARKS_PATH   "/home/root/.bookmarks"
#define HISTORY_PATH "/home/root/.history"

/* --- what is remembered between runs --------------------------------------- */

static void load_list(const char* path, char (*into)[URL_MAX], int* count,
                      char (*titles)[80], int max)
{
    FILE* in = fopen(path, "r");
    if (in == 0)
        return;
    char line[URL_MAX + 96];
    while (*count < max && fgets(line, sizeof(line), in) != 0) {
        unsigned n = (unsigned)strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;
        /* "<url> <title>" - the URL cannot contain a space, so the first one
         * is the divider and everything after it is the name. */
        char* sp = strchr(line, ' ');
        if (sp != 0 && titles != 0) {
            *sp = '\0';
            snprintf(titles[*count], 80, "%s", sp + 1);
        } else if (titles != 0) {
            snprintf(titles[*count], 80, "%s", line);
        }
        snprintf(into[*count], URL_MAX, "%s", line);
        ++*count;
    }
    fclose(in);
}

static void save_marks(void)
{
    FILE* out = fopen(MARKS_PATH, "w");
    if (out == 0)
        return;
    for (int i = 0; i < g_marks_n; ++i)
        fprintf(out, "%s %s\n", g_marks[i], g_mark_title[i]);
    fclose(out);
}

static void save_history(void)
{
    FILE* out = fopen(HISTORY_PATH, "w");
    if (out == 0)
        return;
    /* Only the tail is kept. A history file that grows without bound is a disk
     * leak with a friendly name. */
    const int from = g_history_n > HISTORY_MAX ? g_history_n - HISTORY_MAX : 0;
    for (int i = from; i < g_history_n; ++i)
        fprintf(out, "%s\n", g_history[i]);
    fclose(out);
}

static void remember(const char* url)
{
    if (g_history_n > 0 && strcmp(g_history[g_history_n - 1], url) == 0)
        return;                     /* reloading is not a second visit */
    if (g_history_n >= HISTORY_MAX) {
        for (int i = 1; i < HISTORY_MAX; ++i)
            memcpy(g_history[i - 1], g_history[i], URL_MAX);
        --g_history_n;
    }
    snprintf(g_history[g_history_n++], URL_MAX, "%s", url);
    save_history();
}

/* --- URLs ------------------------------------------------------------------ */

/* Split a URL into host, port and path. Returns 0 on success.
 *
 * Only http is handled, and https is refused by name rather than attempted:
 * there is no TLS here, and a browser that silently fetched an https URL over
 * plain HTTP would be lying about the one thing that scheme promises. */
static int split_url(const char* url, char* host, unsigned* port, char* path)
{
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0)
        return -2;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;

    unsigned n = 0;
    *port = 80;
    while (*p != '\0' && *p != '/' && *p != ':' && n + 1 < 128)
        host[n++] = *p++;
    host[n] = '\0';
    if (n == 0)
        return -1;

    if (*p == ':') {
        ++p;
        unsigned v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (unsigned)(*p++ - '0');
        if (v > 0 && v < 65536)
            *port = v;
    }
    if (*p == '\0') {
        path[0] = '/';
        path[1] = '\0';
    } else {
        snprintf(path, URL_MAX, "%s", p);
    }
    return 0;
}

/* Turn a link's href into something fetchable, against the page it was on. */
static void absolute(const char* base, const char* href, char* out)
{
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        snprintf(out, URL_MAX, "%s", href);
        return;
    }
    char host[128], path[URL_MAX];
    unsigned port = 80;
    if (split_url(base, host, &port, path) != 0) {
        snprintf(out, URL_MAX, "%s", href);
        return;
    }
    if (href[0] == '/') {
        snprintf(out, URL_MAX, "http://%s%s", host, href);
        return;
    }
    /* Relative to the directory the page is in, which is everything up to and
     * including the last slash. */
    char dir[URL_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash != 0)
        slash[1] = '\0';
    else
        dir[0] = '\0';
    snprintf(out, URL_MAX, "http://%s%s%s", host, dir, href);
}

/* --- fetching -------------------------------------------------------------- */

/* Returns the body length, or -1. The status line and headers are consumed
 * here; a redirect is followed by rewriting `url` and reporting -2 so the
 * caller can go round again with a bounded count. */
static long http_get(char* url, char* body, unsigned long max)
{
    char host[128], path[URL_MAX];
    unsigned port = 80;
    const int split = split_url(url, host, &port, path);
    if (split == -2)
        return -3;                  /* https, which this cannot do */
    if (split != 0)
        return -1;

    uint32_t ip = 0;
    if (parse_ip(host, &ip) != 0 && resolve(host, &ip) != 0)
        return -4;                  /* the name did not resolve */

    const int conn = tcp_connect(ip, (uint16_t)port);
    if (conn < 0)
        return -5;

    char request[512];
    /* HTTP/1.0, so the server closes when it is done and the end of the body
     * is the end of the stream - no chunked decoding and no content-length to
     * trust. A browser this size is better off letting the socket say. */
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: %s\r\n"
             "User-Agent: leahOS/web\r\nConnection: close\r\n\r\n", path, host);
    if (tcp_write(conn, request, strlen(request)) < 0) {
        tcp_close(conn);
        return -5;
    }

    unsigned long got = 0;
    for (;;) {
        const long n = tcp_read(conn, body + got, max - 1 - got);
        if (n <= 0)
            break;
        got += (unsigned long)n;
        if (got + 1 >= max)
            break;
    }
    tcp_close(conn);
    body[got] = '\0';
    if (got == 0)
        return -5;

    /* Headers, then a blank line, then the body. */
    char* sep = strstr(body, "\r\n\r\n");
    unsigned skip = 4;
    if (sep == 0) { sep = strstr(body, "\n\n"); skip = 2; }
    if (sep == 0)
        return (long)got;           /* no headers; treat it all as body */

    *sep = '\0';
    /* A redirect is worth following: half the useful URLs are one. */
    if (strncmp(body, "HTTP/1.", 7) == 0) {
        const char* code = body + 9;
        if (code[0] == '3') {
            const char* at = strstr(body, "\nLocation:");
            if (at == 0) at = strstr(body, "\nlocation:");
            if (at != 0) {
                at += 10;
                while (*at == ' ') ++at;
                char to[URL_MAX];
                unsigned n = 0;
                while (*at != '\0' && *at != '\r' && *at != '\n' &&
                       n + 1 < sizeof(to))
                    to[n++] = *at++;
                to[n] = '\0';
                char abs[URL_MAX];
                absolute(url, to, abs);
                snprintf(url, URL_MAX, "%s", abs);
                return -2;
            }
        }
    }

    const unsigned long start = (unsigned long)(sep - body) + skip;
    const unsigned long len = got > start ? got - start : 0;
    memmove(body, body + start, len);
    body[len] = '\0';
    return (long)len;
}

/* --- HTML ------------------------------------------------------------------ */

static int ci_equal(const char* a, const char* b, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* The handful of entities that appear in prose. Anything else is left as
 * written, which is better than dropping it. */
static int entity(const char* p, char* out)
{
    static const struct { const char* name; char ch; } kNamed[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
        { "&quot;", '"' }, { "&apos;", '\'' }, { "&nbsp;", ' ' },
        { "&mdash;", '-' }, { "&ndash;", '-' },
    };
    for (unsigned i = 0; i < sizeof(kNamed) / sizeof(kNamed[0]); ++i) {
        const unsigned n = (unsigned)strlen(kNamed[i].name);
        if (strncmp(p, kNamed[i].name, n) == 0) {
            *out = kNamed[i].ch;
            return (int)n;
        }
    }
    return 0;
}

/* Read the value of an attribute out of a tag body. */
static void attribute(const char* tag, const char* name, char* out,
                      unsigned max)
{
    out[0] = '\0';
    const unsigned nl = (unsigned)strlen(name);
    for (const char* p = tag; *p != '\0'; ++p) {
        if (!ci_equal(p, name, nl))
            continue;
        const char* q = p + nl;
        while (*q == ' ') ++q;
        if (*q != '=')
            continue;
        ++q;
        while (*q == ' ') ++q;
        char quote = '\0';
        if (*q == '"' || *q == '\'') { quote = *q; ++q; }
        unsigned n = 0;
        while (*q != '\0' && n + 1 < max &&
               (quote != '\0' ? *q != quote : (*q != ' ' && *q != '>')))
            out[n++] = *q++;
        out[n] = '\0';
        return;
    }
}

static void parse(struct tab* t, const char* html)
{
    t->runs_n = 0;
    t->links_n = 0;
    t->text_len = 0;
    t->title[0] = '\0';

    unsigned heading = 0, bold = 0, item = 0;
    int link = -1;
    int pending_break = 0;
    int in_title = 0;
    unsigned run_start = 0;
    const unsigned arena_max = PAGE_MAX / 4;

    /* Start a fresh run whenever the style changes, and close the one before
     * it. A run with no text is dropped rather than laid out as a gap. */
    #define FLUSH()                                                        \
        do {                                                               \
            if (t->text_len > run_start && t->runs_n < RUNS_MAX) {         \
                struct run* r = &t->runs[t->runs_n++];                     \
                r->start = run_start; r->len = t->text_len - run_start;    \
                r->heading = (unsigned char)heading;                       \
                r->bold = (unsigned char)bold;                             \
                r->item = (unsigned char)item;                             \
                r->br = (unsigned char)pending_break;                      \
                r->link = link;                                            \
                pending_break = 0; item = 0;                               \
            }                                                              \
            run_start = t->text_len;                                       \
        } while (0)

    for (const char* p = html; *p != '\0'; ) {
        if (*p == '<') {
            const char* end = strchr(p, '>');
            if (end == 0)
                break;
            const char* tag = p + 1;
            const int closing = (*tag == '/');
            if (closing) ++tag;

            /* Script and style hold source code, not prose. Skipped whole,
             * because rendering their contents is the single most obvious way
             * a naive parser announces itself. */
            if (!closing && (ci_equal(tag, "script", 6) ||
                             ci_equal(tag, "style", 5))) {
                const char* close = ci_equal(tag, "script", 6)
                    ? strstr(end, "</script") : strstr(end, "</style");
                if (close == 0) break;
                p = close;
                continue;
            }

            FLUSH();
            if (ci_equal(tag, "title", 5))      in_title = !closing;
            else if (ci_equal(tag, "h1", 2))    heading = closing ? 0 : 1;
            else if (ci_equal(tag, "h2", 2))    heading = closing ? 0 : 2;
            else if (ci_equal(tag, "h3", 2) || ci_equal(tag, "h4", 2))
                                                heading = closing ? 0 : 3;
            else if (ci_equal(tag, "b", 1) || ci_equal(tag, "strong", 6))
                                                bold = closing ? 0 : 1;
            else if (ci_equal(tag, "br", 2))    pending_break = 1;
            else if (ci_equal(tag, "p", 1) || ci_equal(tag, "div", 3))
                                                pending_break = 1;
            else if (ci_equal(tag, "li", 2))  { pending_break = 1; item = !closing; }
            else if (ci_equal(tag, "a", 1)) {
                if (closing) {
                    link = -1;
                } else if (t->links_n < LINKS_MAX) {
                    char href[URL_MAX];
                    attribute(tag, "href", href, sizeof(href));
                    if (href[0] != '\0' && href[0] != '#') {
                        absolute(t->url, href, t->links[t->links_n].href);
                        link = t->links_n++;
                    }
                }
            }
            p = end + 1;
            continue;
        }

        /* Text. Whitespace collapses, which is what HTML says it does and also
         * what stops a source file's indentation becoming the page's. */
        char c = *p;
        int step = 1;
        if (c == '&') {
            char decoded;
            const int n = entity(p, &decoded);
            if (n > 0) { c = decoded; step = n; }
        }
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ' && (t->text_len == 0 ||
                         t->text[t->text_len - 1] == ' ')) {
            p += step;
            continue;
        }
        if (in_title) {
            const unsigned n = (unsigned)strlen(t->title);
            if (n + 1 < sizeof(t->title)) {
                t->title[n] = c;
                t->title[n + 1] = '\0';
            }
        } else if (t->text_len + 1 < arena_max) {
            t->text[t->text_len++] = c;
        }
        p += step;
    }
    FLUSH();
    #undef FLUSH

    if (t->title[0] == '\0')
        snprintf(t->title, sizeof(t->title), "%s", t->url);
}

/* --- loading --------------------------------------------------------------- */

static void load(struct tab* t, const char* url, int record)
{
    if (record && t->url[0] != '\0' && t->back_n < 16)
        snprintf(t->back[t->back_n++], URL_MAX, "%s", t->url);
    snprintf(t->url, URL_MAX, "%s", url);

    snprintf(t->status, sizeof(t->status), "loading %s", url);

    long n = -2;
    for (int hop = 0; hop < 4 && n == -2; ++hop)
        n = http_get(t->url, g_raw, sizeof(g_raw));

    if (n == -3)      snprintf(t->status, sizeof(t->status),
                               "https is not supported - there is no TLS here");
    else if (n == -4) snprintf(t->status, sizeof(t->status),
                               "that name did not resolve");
    else if (n == -5) snprintf(t->status, sizeof(t->status),
                               "could not connect");
    else if (n < 0)   snprintf(t->status, sizeof(t->status), "bad address");
    else {
        parse(t, g_raw);
        snprintf(t->status, sizeof(t->status), "%ld bytes", n);
        t->scroll = 0;
        remember(t->url);
        return;
    }
    t->runs_n = 0;
    snprintf(t->title, sizeof(t->title), "%s", "did not load");
}

static struct tab* cur(void) { return &g_tab[g_cur]; }

static void open_tab(const char* url)
{
    for (int i = 0; i < TABS_MAX; ++i) {
        if (g_tab[i].used)
            continue;
        memset(&g_tab[i], 0, sizeof(g_tab[i]));
        g_tab[i].used = 1;
        g_tab[i].text = g_arena[i];
        g_tab[i].runs = g_runs[i];
        g_tab[i].links = g_links[i];
        g_cur = i;
        snprintf(g_tab[i].title, sizeof(g_tab[i].title), "New tab");
        if (url != 0 && url[0] != '\0')
            load(&g_tab[i], url, 0);
        return;
    }
}

static void close_tab(int i)
{
    if (!g_tab[i].used)
        return;
    g_tab[i].used = 0;
    /* Never no tabs: an empty window with a toolbar is a dead end. */
    int any = -1;
    for (int k = 0; k < TABS_MAX; ++k)
        if (g_tab[k].used) { any = k; break; }
    if (any < 0) { open_tab(0); return; }
    if (g_cur == i) g_cur = any;
}

static void go_back(void)
{
    struct tab* t = cur();
    if (t->back_n <= 0)
        return;
    if (t->forward_n < 16)
        snprintf(t->forward[t->forward_n++], URL_MAX, "%s", t->url);
    load(t, t->back[--t->back_n], 0);
}

static void go_forward(void)
{
    struct tab* t = cur();
    if (t->forward_n <= 0)
        return;
    load(t, t->forward[--t->forward_n], 1);
}

static void bookmark(void)
{
    struct tab* t = cur();
    if (t->url[0] == '\0' || g_marks_n >= MARKS_MAX)
        return;
    for (int i = 0; i < g_marks_n; ++i)
        if (strcmp(g_marks[i], t->url) == 0)
            return;                 /* already kept; adding twice helps nobody */
    snprintf(g_marks[g_marks_n], URL_MAX, "%s", t->url);
    snprintf(g_mark_title[g_marks_n], 80, "%s", t->title);
    ++g_marks_n;
    save_marks();
}

/* --- layout and drawing ---------------------------------------------------- */

/* The page's room, handed over by the layout. */
static struct ui_rect g_page;
static int content_x(void) { return g_page.x; }
static int content_w(void) { return g_page.w - WG_SCROLL_W - 8; }

/* Lay the runs out and draw them, recording where each link landed so a click
 * can be turned back into an href. One pass does both because the layout is
 * cheap and keeping a second copy of it in step is not. */
static int flow(int draw_it)
{
    struct tab* t = cur();
    const int x0 = content_x() + 16;
    const int wide = content_w() - 24;
    int x = x0, y = g_page.y + 12 - t->scroll;
    int line_h = WG_GLYPH_H + 4;

    for (int i = 0; i < t->links_n; ++i)
        t->links[i].w = 0;

    for (int i = 0; i < t->runs_n; ++i) {
        const struct run* r = &t->runs[i];
        /* A heading is drawn larger and bold is drawn bold.
         *
         * `scale` used to double the *width* a heading's words were given and
         * nothing else - they were still drawn at the ordinary size, so a
         * heading was normal text with a gap after every word. And `bold` only
         * changed the colour, on a page whose ordinary ink is that colour. The
         * toolkit can do both now, and one size is used to measure and to
         * draw, so they cannot disagree. */
        const int px = r->heading == 1 ? wg_text_size() * 2
                     : r->heading      ? wg_text_size() * 3 / 2
                                       : wg_text_size();
        const unsigned style = (r->bold || r->heading) ? WG_STYLE_BOLD : 0;
        line_h = wg_styled_height(px) + (r->heading ? 6 : 2);

        if (r->br && x > x0) { x = x0; y += line_h; }
        if (r->heading && x > x0) { x = x0; y += line_h; }
        if (r->item) {
            if (draw_it && y > g_page.y - line_h && y < g_page.y + g_page.h)
                wg_text(x0 - 10, y, "-", WG_DIM);
        }

        /* Word by word, wrapping at the margin. */
        unsigned at = r->start;
        const unsigned end = r->start + r->len;
        while (at < end) {
            unsigned wend = at;
            while (wend < end && t->text[wend] != ' ') ++wend;
            char word[128];
            unsigned n = wend - at;
            if (n > sizeof(word) - 1) n = sizeof(word) - 1;
            memcpy(word, t->text + at, n);
            word[n] = '\0';

            const int ww = wg_styled_width(word, (int)n, px, style);
            if (x + ww > x0 + wide && x > x0) { x = x0; y += line_h; }

            if (draw_it && y > g_page.y - line_h && y < g_page.y + g_page.h) {
                /* Underlined, because colour alone is not a link on a theme
                 * whose accent happens to be close to the ink. */
                const unsigned how = style |
                    (r->link >= 0 ? WG_STYLE_UNDERLINE : 0u);
                wg_styled(x, y, word, (int)n,
                          r->link >= 0 ? WG_ACCENT : wg_ink_colour(), px, how);
                if (r->link >= 0) {
                    struct link* l = &t->links[r->link];
                    if (l->w == 0) { l->x = x; l->y = y;
                                     l->h = wg_styled_height(px); }
                    l->w = (x + ww) - l->x;
                }
            }
            x += ww + wg_styled_width(" ", 1, px, style);
            at = wend + 1;
        }
    }
    return y + t->scroll + 40;      /* the document's height */
}

/* --- the interface ---------------------------------------------------------
 *
 * Tabs, a toolbar and a sidebar are components; the page is a custom view,
 * because laying out a document is what this program is and no component does
 * it. What the port buys is that the page's room comes from the layout, so the
 * sidebar's width stops being a number this file repeats.
 */

static struct app g_app;
static struct ui_view* g_tabbar;
static struct ui_view* g_urlfield;
static struct ui_view* g_sidelist;
static struct ui_view* g_status;
static char g_status_text[128];

static const char* tab_title(void* user, int row)
{
    (void)user;
    int seen = 0;
    for (int i = 0; i < TABS_MAX; ++i) {
        if (!g_tab[i].used)
            continue;
        if (seen == row)
            return g_tab[i].title;
        ++seen;
    }
    return "";
}

static int tab_count(void)
{
    int n = 0;
    for (int i = 0; i < TABS_MAX; ++i)
        if (g_tab[i].used) ++n;
    return n;
}

static const char* side_row(void* user, int row)
{
    (void)user;
    if (g_side == SIDE_MARKS)
        return (row >= 0 && row < g_marks_n) ? g_mark_title[row] : "";
    return (row >= 0 && row < g_history_n) ? g_history[g_history_n - 1 - row]
                                           : "";
}

static void sync_chrome(void)
{
    g_tabbar->rows = tab_count();
    ui_set_text(g_urlfield, cur()->url);
    snprintf(g_status_text, sizeof(g_status_text), "%s", cur()->status);
    ui_set_text(g_status, g_status_text);
    if (g_side == SIDE_NONE) {
        g_sidelist->flags |= UI_HIDDEN;
    } else {
        g_sidelist->flags &= ~UI_HIDDEN;
        g_sidelist->rows = g_side == SIDE_MARKS ? g_marks_n : g_history_n;
    }
    app_relayout(&g_app);
}

static void draw_page(struct ui_view* v, void* user)
{
    (void)user;
    g_page = v->frame;
    flow(1);
    struct tab* t = cur();
    const int height = flow(0);
    if (height > g_page.h)
        wg_scrollbar_v(g_page.x + g_page.w - WG_SCROLL_W, g_page.y, g_page.h,
                       t->scroll, g_page.h, height);
}

static void on_tab(struct ui_view* v, void* user)
{
    (void)user;
    int seen = 0;
    for (int i = 0; i < TABS_MAX; ++i) {
        if (!g_tab[i].used)
            continue;
        if (seen == v->on) { g_cur = i; break; }
        ++seen;
    }
    sync_chrome();
}

static void on_url(struct ui_view* v, void* user)
{
    (void)user;
    load(cur(), ui_text(v), 1);
    sync_chrome();
}

static void on_back(struct ui_view* v, void* u)    { (void)v; (void)u; go_back(); sync_chrome(); }
static void on_forward(struct ui_view* v, void* u) { (void)v; (void)u; go_forward(); sync_chrome(); }
static void on_reload(struct ui_view* v, void* u)  { (void)v; (void)u; load(cur(), cur()->url, 0); sync_chrome(); }
static void on_newtab(struct ui_view* v, void* u)  { (void)v; (void)u; open_tab(0); sync_chrome(); }
static void on_bookmark(struct ui_view* v, void* u){ (void)v; (void)u; bookmark(); sync_chrome(); }

static void on_marks(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    g_side = g_side == SIDE_MARKS ? SIDE_NONE : SIDE_MARKS;
    sync_chrome();
}

static void on_history(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    g_side = g_side == SIDE_HISTORY ? SIDE_NONE : SIDE_HISTORY;
    sync_chrome();
}

static void on_side(struct ui_view* v, void* user)
{
    (void)user;
    const int row = v->selected;
    if (row < 0)
        return;
    if (g_side == SIDE_MARKS && row < g_marks_n)
        load(cur(), g_marks[row], 1);
    else if (g_side == SIDE_HISTORY && row < g_history_n)
        load(cur(), g_history[g_history_n - 1 - row], 1);
    sync_chrome();
}

static int on_event(struct app* a, const struct win_event* e)
{
    (void)a;
    struct tab* t = cur();
    if (e->type == WIN_EVENT_MOUSE_DOWN &&
        e->x >= g_page.x && e->y >= g_page.y &&
        e->y < g_page.y + g_page.h) {
        for (int i = 0; i < t->links_n; ++i) {
            const struct link* l = &t->links[i];
            if (l->w > 0 && e->x >= l->x && e->x < l->x + l->w &&
                e->y >= l->y && e->y < l->y + l->h) {
                load(t, l->href, 1);
                sync_chrome();
                return 1;
            }
        }
        return 0;
    }
    if (e->type == WIN_EVENT_KEY) {
        if (e->key == WIN_KEY_DOWN)       t->scroll += 40;
        else if (e->key == WIN_KEY_UP)    t->scroll = t->scroll > 40 ? t->scroll - 40 : 0;
        else return 0;
        return 1;
    }
    return 0;
}

static const char* const kMenu[] = { "New tab", "Close tab", "-",
                                     "Bookmark this page", "Reload" };

static int on_menu(struct app* a, int pick)
{
    (void)a;
    if (pick == 0)      open_tab(0);
    else if (pick == 1) close_tab(g_cur);
    else if (pick == 3) bookmark();
    else if (pick == 4) load(cur(), cur()->url, 0);
    sync_chrome();
    return 1;
}

int main(int argc, char** argv)
{
    load_list(MARKS_PATH, g_marks, &g_marks_n, g_mark_title, MARKS_MAX);
    load_list(HISTORY_PATH, g_history, &g_history_n, 0, HISTORY_MAX);
    open_tab(argc > 1 && argv[1][0] != '-' ? argv[1] : 0);

    struct ui_view* root = ui_box(0, UI_STACK_V, 0, 0);

    g_tabbar = ui_tabs(root, tab_title, tab_count(), 0);
    ui_on(g_tabbar, on_tab, 0);
    ui_grow(g_tabbar, 0);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 8, 6);
    ui_size(bar, 0, 38);
    ui_grow(bar, 0);
    ui_grow(ui_button(bar, "<", on_back, 0), 0);
    ui_grow(ui_button(bar, ">", on_forward, 0), 0);
    ui_grow(ui_button(bar, "R", on_reload, 0), 0);
    ui_grow(ui_button(bar, "+", on_newtab, 0), 0);
    g_urlfield = ui_field(bar, "");
    ui_on(g_urlfield, on_url, 0);
    ui_grow(g_urlfield, 1);
    ui_grow(ui_button(bar, "Bookmark", on_bookmark, 0), 0);
    ui_grow(ui_button(bar, "Marks", on_marks, 0), 0);
    ui_grow(ui_button(bar, "History", on_history, 0), 0);

    struct ui_view* body = ui_box(root, UI_STACK_H, 0, 0);
    g_sidelist = ui_sidebar(body, side_row, 0, 0);
    ui_on(g_sidelist, on_side, 0);
    ui_size(g_sidelist, SIDE_W, 0);
    g_sidelist->flags |= UI_HIDDEN;
    ui_custom(body, draw_page, 0);

    g_status = ui_label(root, "");
    ui_grow(g_status, 0);
    sync_chrome();

    g_app.title = "Web";
    g_app.width = 900; g_app.height = 600;
    g_app.min_width = 560; g_app.min_height = 360;
    g_app.event = on_event;
    g_app.menu = kMenu;
    g_app.menu_count = 5;
    g_app.menu_pick = on_menu;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
