/* Reading and writing RTF. See rtf.h for what the document is and why.
 *
 * The reader is a state machine over three things: braces, which push and pop
 * that state; control words, which change it; and everything else, which is
 * text. That is the whole of RTF's grammar. The complexity in real files is in
 * the number of control words, not in the shape of them - so the reader
 * recognises the dozen that mean something here and steps over the rest, and
 * skips whole groups whose destination it does not know.
 */

#include <errno.h>
#include <fcntl.h>
#include <rtf.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const int rtf_size_points[RTF_SIZES] = { 8, 10, 12, 14, 18, 24, 32, 48 };

/* --- the document ---------------------------------------------------------- */

struct rtf_doc* rtf_new(void)
{
    struct rtf_doc* d = (struct rtf_doc*)malloc(sizeof(struct rtf_doc));
    if (d == 0)
        return 0;
    d->cap = 256;
    d->text = (char*)malloc((unsigned long)d->cap);
    d->style = (unsigned char*)malloc((unsigned long)d->cap);
    if (d->text == 0 || d->style == 0) {
        free(d->text);
        free(d->style);
        free(d);
        return 0;
    }
    d->len = 0;
    d->align = RTF_LEFT;
    return d;
}

void rtf_free(struct rtf_doc* d)
{
    if (d == 0)
        return;
    free(d->text);
    free(d->style);
    free(d);
}

/* Room for `extra` more characters. Doubling rather than growing by what was
 * asked for: typing is one character at a time, and a reallocation per
 * keystroke is a copy of the whole document per keystroke. */
static int reserve(struct rtf_doc* d, long extra)
{
    if (d->len + extra <= d->cap)
        return 0;
    long want = d->cap * 2;
    while (want < d->len + extra)
        want *= 2;
    char* text = (char*)malloc((unsigned long)want);
    unsigned char* style = (unsigned char*)malloc((unsigned long)want);
    if (text == 0 || style == 0) {
        free(text);
        free(style);
        errno = ENOMEM;
        return -1;
    }
    memcpy(text, d->text, (unsigned long)d->len);
    memcpy(style, d->style, (unsigned long)d->len);
    free(d->text);
    free(d->style);
    d->text = text;
    d->style = style;
    d->cap = want;
    return 0;
}

int rtf_insert(struct rtf_doc* d, long at, const char* s, long n,
               unsigned char style)
{
    if (d == 0 || n <= 0)
        return 0;
    if (at < 0) at = 0;
    if (at > d->len) at = d->len;
    if (reserve(d, n) != 0)
        return -1;
    memmove(&d->text[at + n], &d->text[at], (unsigned long)(d->len - at));
    memmove(&d->style[at + n], &d->style[at], (unsigned long)(d->len - at));
    memcpy(&d->text[at], s, (unsigned long)n);
    memset(&d->style[at], style, (unsigned long)n);
    d->len += n;
    return 0;
}

int rtf_delete(struct rtf_doc* d, long at, long n)
{
    if (d == 0 || n <= 0 || at < 0 || at >= d->len)
        return 0;
    if (at + n > d->len)
        n = d->len - at;
    memmove(&d->text[at], &d->text[at + n],
            (unsigned long)(d->len - at - n));
    memmove(&d->style[at], &d->style[at + n],
            (unsigned long)(d->len - at - n));
    d->len -= n;
    return 0;
}

static void clamp_range(const struct rtf_doc* d, long* from, long* to)
{
    if (*from > *to) { const long t = *from; *from = *to; *to = t; }
    if (*from < 0) *from = 0;
    if (*to > d->len) *to = d->len;
}

void rtf_restyle(struct rtf_doc* d, long from, long to, unsigned flags, int on)
{
    if (d == 0)
        return;
    clamp_range(d, &from, &to);
    for (long i = from; i < to; ++i)
        d->style[i] = on ? (unsigned char)(d->style[i] | flags)
                         : (unsigned char)(d->style[i] & ~flags);
}

void rtf_resize(struct rtf_doc* d, long from, long to, unsigned index)
{
    if (d == 0 || index >= RTF_SIZES)
        return;
    clamp_range(d, &from, &to);
    for (long i = from; i < to; ++i)
        d->style[i] = rtf_style_with_size(d->style[i], index);
}

unsigned char rtf_style_at(const struct rtf_doc* d, long from, long to)
{
    if (d == 0 || d->len == 0)
        return (unsigned char)(RTF_SIZE_DEFAULT << RTF_SIZE_SHIFT);
    long a = from, b = to;
    clamp_range(d, &a, &b);
    if (a == b) {
        /* A caret takes the style of the character before it, which is what
         * makes typing continue whatever was just typed rather than reverting
         * at every gap. At the very start there is nothing before it. */
        const long at = a > 0 ? a - 1 : 0;
        return d->style[at < d->len ? at : d->len - 1];
    }
    unsigned char common = d->style[a];
    for (long i = a + 1; i < b; ++i) {
        const unsigned char s = d->style[i];
        /* A flag survives only if every character has it. The size survives
         * only if they all agree; otherwise it is reported as the first
         * character's, because "several sizes" is not a size a menu can show. */
        common = (unsigned char)((common & s & RTF_FLAGS) | (common & RTF_SIZE_MASK));
        if (rtf_style_size(s) != rtf_style_size(common))
            common = (unsigned char)(common & ~RTF_SIZE_MASK);
    }
    return common;
}

/* --- reading --------------------------------------------------------------- */

/* Half-points, as RTF counts them, to the nearest size this can show. */
static unsigned size_index_for(int half_points)
{
    const int points = half_points / 2;
    unsigned best = RTF_SIZE_DEFAULT;
    int best_gap = 1000;
    for (unsigned i = 0; i < RTF_SIZES; ++i) {
        int gap = rtf_size_points[i] - points;
        if (gap < 0) gap = -gap;
        if (gap < best_gap) { best_gap = gap; best = i; }
    }
    return best;
}

/* The reader's state, one per open brace. RTF's groups are a stack of exactly
 * this, which is why `{\b bold}` leaves bold behind when the group closes. */
struct state {
    unsigned char style;
    int           align;
    int           skip;     /* inside a destination we do not understand */
};

#define RTF_DEPTH 32

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Everything that is not RTF at all, taken as plain text. Opening a .txt by
 * mistake should show the text, not an error. */
static struct rtf_doc* as_plain(const char* buf, long n)
{
    struct rtf_doc* d = rtf_new();
    if (d == 0)
        return 0;
    const unsigned char style =
        (unsigned char)(RTF_SIZE_DEFAULT << RTF_SIZE_SHIFT);
    for (long i = 0; i < n; ++i) {
        if (buf[i] == '\r')
            continue;
        if (rtf_insert(d, d->len, &buf[i], 1, style) != 0) {
            rtf_free(d);
            return 0;
        }
    }
    return d;
}

struct rtf_doc* rtf_read(const char* path)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    /* Read it whole. A document this program can edit fits in memory by
     * definition - it is all in one array while it is open. */
    long cap = 4096, n = 0;
    char* buf = (char*)malloc((unsigned long)cap);
    if (buf == 0) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }
    for (;;) {
        if (n == cap) {
            char* grown = (char*)malloc((unsigned long)cap * 2);
            if (grown == 0) {
                free(buf);
                close(fd);
                errno = ENOMEM;
                return 0;
            }
            memcpy(grown, buf, (unsigned long)n);
            free(buf);
            buf = grown;
            cap *= 2;
        }
        const long got = read(fd, &buf[n], (unsigned long)(cap - n));
        if (got <= 0)
            break;
        n += got;
    }
    close(fd);

    if (n < 5 || memcmp(buf, "{\\rtf", 5) != 0) {
        struct rtf_doc* plain = as_plain(buf, n);
        free(buf);
        return plain;
    }

    struct rtf_doc* d = rtf_new();
    if (d == 0) {
        free(buf);
        return 0;
    }

    struct state stack[RTF_DEPTH];
    int depth = 0;
    stack[0].style = (unsigned char)(RTF_SIZE_DEFAULT << RTF_SIZE_SHIFT);
    stack[0].align = RTF_LEFT;
    stack[0].skip = 0;

    long i = 0;
    while (i < n) {
        const char c = buf[i];

        if (c == '{') {
            if (depth + 1 < RTF_DEPTH) {
                stack[depth + 1] = stack[depth];
                ++depth;
            }
            ++i;
            continue;
        }
        if (c == '}') {
            if (depth > 0)
                --depth;
            ++i;
            continue;
        }
        if (c == '\\') {
            ++i;
            if (i >= n)
                break;
            const char k = buf[i];

            /* An escaped character rather than a control word. */
            if (k == '\\' || k == '{' || k == '}') {
                if (!stack[depth].skip)
                    rtf_insert(d, d->len, &k, 1, stack[depth].style);
                ++i;
                continue;
            }
            if (k == '\'') {
                /* \'hh - one byte, in whatever code page. Taken as Latin-1,
                 * which is what \ansi means and what anything this reads will
                 * have used. */
                const int hi = i + 1 < n ? hex_value(buf[i + 1]) : -1;
                const int lo = i + 2 < n ? hex_value(buf[i + 2]) : -1;
                i += 3;
                if (hi >= 0 && lo >= 0 && !stack[depth].skip) {
                    const unsigned v = (unsigned)(hi * 16 + lo);
                    if (v < 0x80) {
                        const char ch = (char)v;
                        rtf_insert(d, d->len, &ch, 1, stack[depth].style);
                    } else {
                        /* Two bytes of UTF-8, since that is what the rest of
                         * this system reads. */
                        const char two[2] = { (char)(0xC0 | (v >> 6)),
                                              (char)(0x80 | (v & 0x3F)) };
                        rtf_insert(d, d->len, two, 2, stack[depth].style);
                    }
                }
                continue;
            }
            if (k == '*') {
                /* \*\something - a destination whose contents are only
                 * meaningful to a reader that knows it. Skipping the group is
                 * what the specification asks of everyone else. */
                stack[depth].skip = 1;
                ++i;
                continue;
            }
            if (k == '\n' || k == '\r') {
                ++i;
                continue;
            }

            /* A control word: letters, then an optional signed number, then
             * one optional space which belongs to the word rather than to the
             * text. */
            char word[32];
            int w = 0;
            while (i < n && ((buf[i] >= 'a' && buf[i] <= 'z') ||
                             (buf[i] >= 'A' && buf[i] <= 'Z'))) {
                if (w + 1 < (int)sizeof(word))
                    word[w++] = buf[i];
                ++i;
            }
            word[w] = '\0';

            int has_number = 0, negative = 0;
            long number = 0;
            if (i < n && buf[i] == '-') { negative = 1; ++i; }
            while (i < n && buf[i] >= '0' && buf[i] <= '9') {
                has_number = 1;
                number = number * 10 + (buf[i] - '0');
                ++i;
            }
            if (negative)
                number = -number;
            if (i < n && buf[i] == ' ')
                ++i;

            if (w == 0)
                continue;

            /* Destinations whose text is not the document's text. The font and
             * colour tables, the header, and the whole of \info. */
            if (strcmp(word, "fonttbl") == 0 || strcmp(word, "colortbl") == 0 ||
                strcmp(word, "stylesheet") == 0 || strcmp(word, "info") == 0 ||
                strcmp(word, "pict") == 0) {
                stack[depth].skip = 1;
                continue;
            }

            if (stack[depth].skip)
                continue;

            const int off = has_number && number == 0;
            if (strcmp(word, "b") == 0) {
                stack[depth].style = off
                    ? (unsigned char)(stack[depth].style & ~RTF_BOLD)
                    : (unsigned char)(stack[depth].style | RTF_BOLD);
            } else if (strcmp(word, "i") == 0) {
                stack[depth].style = off
                    ? (unsigned char)(stack[depth].style & ~RTF_ITALIC)
                    : (unsigned char)(stack[depth].style | RTF_ITALIC);
            } else if (strcmp(word, "ul") == 0) {
                stack[depth].style = (unsigned char)(stack[depth].style | RTF_UNDERLINE);
            } else if (strcmp(word, "ulnone") == 0) {
                stack[depth].style = (unsigned char)(stack[depth].style & ~RTF_UNDERLINE);
            } else if (strcmp(word, "fs") == 0 && has_number) {
                stack[depth].style =
                    rtf_style_with_size(stack[depth].style,
                                        size_index_for((int)number));
            } else if (strcmp(word, "ql") == 0) {
                stack[depth].align = RTF_LEFT;   d->align = RTF_LEFT;
            } else if (strcmp(word, "qc") == 0) {
                stack[depth].align = RTF_CENTRE; d->align = RTF_CENTRE;
            } else if (strcmp(word, "qr") == 0) {
                stack[depth].align = RTF_RIGHT;  d->align = RTF_RIGHT;
            } else if (strcmp(word, "par") == 0 || strcmp(word, "line") == 0) {
                const char nl = '\n';
                rtf_insert(d, d->len, &nl, 1, stack[depth].style);
            } else if (strcmp(word, "tab") == 0) {
                const char tab = '\t';
                rtf_insert(d, d->len, &tab, 1, stack[depth].style);
            } else if (strcmp(word, "plain") == 0) {
                stack[depth].style =
                    (unsigned char)(RTF_SIZE_DEFAULT << RTF_SIZE_SHIFT);
            }
            continue;
        }

        /* Ordinary text. Newlines in the file are not newlines in the
         * document - RTF says so with \par - so they are whitespace here. */
        if (c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (!stack[depth].skip)
            rtf_insert(d, d->len, &c, 1, stack[depth].style);
        ++i;
    }

    free(buf);
    return d;
}

/* --- writing --------------------------------------------------------------- */

struct out {
    int   fd;
    char  buf[1024];
    int   n;
    int   failed;
};

static void flush(struct out* o)
{
    if (o->n == 0 || o->failed)
        return;
    if (write(o->fd, o->buf, (unsigned long)o->n) != o->n)
        o->failed = 1;
    o->n = 0;
}

static void put(struct out* o, const char* s, int n)
{
    for (int i = 0; i < n; ++i) {
        if (o->n == (int)sizeof(o->buf))
            flush(o);
        o->buf[o->n++] = s[i];
    }
}

static void puts_(struct out* o, const char* s)
{
    put(o, s, (int)strlen(s));
}

static void put_number(struct out* o, long v)
{
    char digits[24];
    int n = 0;
    if (v < 0) { puts_(o, "-"); v = -v; }
    if (v == 0) digits[n++] = '0';
    while (v > 0) { digits[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0)
        put(o, &digits[--n], 1);
}

int rtf_write(const char* path, const struct rtf_doc* d)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    struct out o;
    o.fd = fd;
    o.n = 0;
    o.failed = 0;

    puts_(&o, "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}}\n");
    puts_(&o, d->align == RTF_CENTRE ? "\\qc" :
              d->align == RTF_RIGHT  ? "\\qr" : "\\ql");

    /* The state as last written. A control word is emitted only where it
     * differs, which is what makes the file a sequence of runs without this
     * ever having to know what a run is. */
    unsigned char now = (unsigned char)(RTF_SIZE_DEFAULT << RTF_SIZE_SHIFT);
    puts_(&o, "\\fs");
    put_number(&o, rtf_size_points[RTF_SIZE_DEFAULT] * 2);
    puts_(&o, " ");

    for (long i = 0; i < d->len; ++i) {
        const unsigned char want = d->style[i];
        if ((want & RTF_BOLD) != (now & RTF_BOLD))
            puts_(&o, (want & RTF_BOLD) ? "\\b " : "\\b0 ");
        if ((want & RTF_ITALIC) != (now & RTF_ITALIC))
            puts_(&o, (want & RTF_ITALIC) ? "\\i " : "\\i0 ");
        if ((want & RTF_UNDERLINE) != (now & RTF_UNDERLINE))
            puts_(&o, (want & RTF_UNDERLINE) ? "\\ul " : "\\ulnone ");
        if (rtf_style_size(want) != rtf_style_size(now)) {
            puts_(&o, "\\fs");
            put_number(&o, rtf_size_points[rtf_style_size(want)] * 2);
            puts_(&o, " ");
        }
        now = want;

        const char c = d->text[i];
        if (c == '\n')       puts_(&o, "\\par\n");
        else if (c == '\t')  puts_(&o, "\\tab ");
        else if (c == '\\')  puts_(&o, "\\\\");
        else if (c == '{')   puts_(&o, "\\{");
        else if (c == '}')   puts_(&o, "\\}");
        else if ((unsigned char)c >= 0x80) {
            /* Out as a hex escape, which every reader understands, rather than
             * as the UTF-8 bytes - those would be read back as two Latin-1
             * characters by anything following \ansi. */
            static const char kHex[] = "0123456789abcdef";
            puts_(&o, "\\'");
            put(&o, &kHex[((unsigned char)c >> 4) & 0xF], 1);
            put(&o, &kHex[(unsigned char)c & 0xF], 1);
        }
        else put(&o, &c, 1);
    }
    puts_(&o, "}\n");
    flush(&o);
    if (close(fd) != 0)
        o.failed = 1;
    return o.failed ? -1 : 0;
}
