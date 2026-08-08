/* Regular expressions: a parser and a backtracking matcher.
 *
 * The grammar, which is the usual one and is what makes the parser four
 * functions rather than a state machine:
 *
 *   alternation := concat ('|' concat)*
 *   concat      := repeat*
 *   repeat      := atom ('*' | '+' | '?' | '{n,m}')?
 *   atom        := '(' alternation ')' | '[' set ']' | '.' | '^' | '$'
 *                | '\' escape | literal
 *
 * Matching is by continuation: each node is asked to match at a position and,
 * if it can, to ask the rest of the pattern to match at what is left. That is
 * what makes a quantifier's backtracking fall out for free - it tries the
 * longest run first and hands back one character at a time until the rest of
 * the pattern is happy.
 *
 * Nodes come from one array owned by the compiled pattern, so freeing it is
 * one call and there is no tree to walk. Children are indices, not pointers,
 * for the same reason.
 */

#include <regex.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 512
#define MAX_STEPS 200000        /* the guard against a pathological pattern */

enum node_kind {
    N_EMPTY,        /* matches nothing at all, successfully */
    N_CHAR,         /* one literal character */
    N_ANY,          /* . */
    N_SET,          /* [...] */
    N_BEGIN,        /* ^ */
    N_END,          /* $ */
    N_CONCAT,       /* left then right */
    N_ALT,          /* left or right */
    N_REPEAT,       /* left, between min and max times */
};

struct node {
    unsigned char kind;
    char          ch;           /* N_CHAR */
    int           left, right;  /* children, or -1 */
    int           min, max;     /* N_REPEAT; max -1 means no limit */
    unsigned char set[32];      /* N_SET, one bit per byte value */
    unsigned char negate;
};

struct regex {
    struct node nodes[MAX_NODES];
    int         count;
    int         root;
    int         ignore_case;
    long        steps;
};

/* --- parsing ------------------------------------------------------------- */

struct parser {
    struct regex* re;
    const char*   at;
    const char*   error;
};

static int parse_alternation(struct parser* p);

static int new_node(struct parser* p, enum node_kind kind)
{
    if (p->re->count >= MAX_NODES) {
        if (p->error == 0)
            p->error = "the pattern is too complicated";
        return -1;
    }
    const int at = p->re->count++;
    struct node* n = &p->re->nodes[at];
    memset(n, 0, sizeof(*n));
    n->kind = (unsigned char)kind;
    n->left = n->right = -1;
    n->min = n->max = 1;
    return at;
}

static void set_add(struct node* n, unsigned char c)
{
    n->set[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static void set_add_range(struct node* n, unsigned char from, unsigned char to)
{
    for (unsigned c = from; c <= to; ++c)
        set_add(n, (unsigned char)c);
}

/* The named classes, which are the ones people actually type. */
static void set_add_class(struct node* n, char which)
{
    switch (which) {
    case 'd': set_add_range(n, '0', '9'); break;
    case 'w':
        set_add_range(n, 'a', 'z');
        set_add_range(n, 'A', 'Z');
        set_add_range(n, '0', '9');
        set_add(n, '_');
        break;
    case 's':
        set_add(n, ' '); set_add(n, '\t'); set_add(n, '\n');
        set_add(n, '\r'); set_add(n, '\f'); set_add(n, '\v');
        break;
    default: break;
    }
}

static char escaped_char(char c)
{
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    default:  return c;
    }
}

static int parse_set(struct parser* p)
{
    const int at = new_node(p, N_SET);
    if (at < 0)
        return -1;
    struct node* n = &p->re->nodes[at];

    if (*p->at == '^') {
        n->negate = 1;
        ++p->at;
    }
    /* A ] first is a literal ], which is how a set containing one is written
     * and why this is checked before the loop rather than inside it. */
    if (*p->at == ']') {
        set_add(n, ']');
        ++p->at;
    }

    while (*p->at != '\0' && *p->at != ']') {
        unsigned char c = (unsigned char)*p->at++;
        if (c == '\\' && *p->at != '\0') {
            const char which = *p->at++;
            if (which == 'd' || which == 'w' || which == 's') {
                set_add_class(n, which);
                continue;
            }
            if (which == 'D' || which == 'W' || which == 'S') {
                /* An inverted class inside a set cannot be expressed as bits
                 * without inverting the whole set, so it is refused rather
                 * than quietly meaning something else. */
                p->error = "an inverted class does not fit inside [ ]";
                return -1;
            }
            c = (unsigned char)escaped_char(which);
        }
        if (*p->at == '-' && p->at[1] != ']' && p->at[1] != '\0') {
            ++p->at;
            unsigned char to = (unsigned char)*p->at++;
            if (to == '\\' && *p->at != '\0')
                to = (unsigned char)escaped_char(*p->at++);
            if (to < c) {
                p->error = "a range that runs backwards";
                return -1;
            }
            set_add_range(n, c, to);
        } else {
            set_add(n, c);
        }
    }
    if (*p->at != ']') {
        p->error = "unmatched [";
        return -1;
    }
    ++p->at;

    /* Case folding is done here, once, rather than on every character of every
     * line: a set that contains 'a' is given 'A' as well. */
    if (p->re->ignore_case) {
        for (unsigned c = 'a'; c <= 'z'; ++c) {
            const int lower = (n->set[c >> 3] >> (c & 7)) & 1;
            const unsigned upper = c - 'a' + 'A';
            const int has_upper = (n->set[upper >> 3] >> (upper & 7)) & 1;
            if (lower) set_add(n, (unsigned char)upper);
            if (has_upper) set_add(n, (unsigned char)c);
        }
    }
    return at;
}

static int parse_atom(struct parser* p)
{
    if (*p->at == '(') {
        ++p->at;
        const int inner = parse_alternation(p);
        if (inner < 0)
            return -1;
        if (*p->at != ')') {
            p->error = "unmatched (";
            return -1;
        }
        ++p->at;
        return inner;
    }
    if (*p->at == '[') {
        ++p->at;
        return parse_set(p);
    }
    if (*p->at == '.') {
        ++p->at;
        return new_node(p, N_ANY);
    }
    if (*p->at == '^') {
        ++p->at;
        return new_node(p, N_BEGIN);
    }
    if (*p->at == '$') {
        ++p->at;
        return new_node(p, N_END);
    }
    if (*p->at == '\\') {
        ++p->at;
        if (*p->at == '\0') {
            p->error = "a trailing backslash";
            return -1;
        }
        const char which = *p->at++;
        if (which == 'd' || which == 'w' || which == 's' ||
            which == 'D' || which == 'W' || which == 'S') {
            const int at = new_node(p, N_SET);
            if (at < 0)
                return -1;
            struct node* n = &p->re->nodes[at];
            set_add_class(n, which >= 'A' && which <= 'Z'
                                 ? which - 'A' + 'a' : which);
            n->negate = which >= 'A' && which <= 'Z';
            return at;
        }
        const int at = new_node(p, N_CHAR);
        if (at < 0)
            return -1;
        p->re->nodes[at].ch = escaped_char(which);
        return at;
    }

    const int at = new_node(p, N_CHAR);
    if (at < 0)
        return -1;
    p->re->nodes[at].ch = *p->at++;
    return at;
}

/* {n}, {n,} and {n,m}. A brace that is not one of those is a literal brace,
 * which is what every implementation does and what people rely on. */
static int parse_braces(struct parser* p, int* min, int* max)
{
    const char* mark = p->at;
    const char* at = p->at + 1;
    int low = 0, high = -1, digits = 0;

    while (*at >= '0' && *at <= '9') { low = low * 10 + (*at++ - '0'); ++digits; }
    if (digits == 0) { p->at = mark; return 0; }
    if (*at == ',') {
        ++at;
        int hdigits = 0, value = 0;
        while (*at >= '0' && *at <= '9') { value = value * 10 + (*at++ - '0'); ++hdigits; }
        high = hdigits > 0 ? value : -1;
    } else {
        high = low;
    }
    if (*at != '}') { p->at = mark; return 0; }
    if (high >= 0 && high < low) {
        p->error = "a count that runs backwards";
        return -1;
    }
    p->at = at + 1;
    *min = low;
    *max = high;
    return 1;
}

static int parse_repeat(struct parser* p)
{
    int atom = parse_atom(p);
    if (atom < 0)
        return -1;

    for (;;) {
        int min, max;
        if (*p->at == '*')      { min = 0; max = -1; ++p->at; }
        else if (*p->at == '+') { min = 1; max = -1; ++p->at; }
        else if (*p->at == '?') { min = 0; max = 1;  ++p->at; }
        else if (*p->at == '{') {
            const int got = parse_braces(p, &min, &max);
            if (got < 0)
                return -1;
            if (got == 0)
                break;
        } else {
            break;
        }
        const int at = new_node(p, N_REPEAT);
        if (at < 0)
            return -1;
        p->re->nodes[at].left = atom;
        p->re->nodes[at].min = min;
        p->re->nodes[at].max = max;
        atom = at;
    }
    return atom;
}

static int parse_concat(struct parser* p)
{
    int left = -1;
    while (*p->at != '\0' && *p->at != '|' && *p->at != ')') {
        const int next = parse_repeat(p);
        if (next < 0)
            return -1;
        if (left < 0) {
            left = next;
            continue;
        }
        const int at = new_node(p, N_CONCAT);
        if (at < 0)
            return -1;
        p->re->nodes[at].left = left;
        p->re->nodes[at].right = next;
        left = at;
    }
    return left >= 0 ? left : new_node(p, N_EMPTY);
}

static int parse_alternation(struct parser* p)
{
    int left = parse_concat(p);
    if (left < 0)
        return -1;
    while (*p->at == '|') {
        ++p->at;
        const int right = parse_concat(p);
        if (right < 0)
            return -1;
        const int at = new_node(p, N_ALT);
        if (at < 0)
            return -1;
        p->re->nodes[at].left = left;
        p->re->nodes[at].right = right;
        left = at;
    }
    return left;
}

/* --- matching ------------------------------------------------------------ */

/* What to do once a node has matched. A linked list of nodes still to satisfy,
 * built on the C stack as the matcher descends - which is what lets a
 * quantifier hand control to the rest of the pattern without knowing what the
 * rest of the pattern is.
 *
 * A node of -2 is not a node: it marks a continuation that belongs to a
 * repeat, and says "one iteration finished here; decide whether to go round
 * again". The struct it is embedded in carries what that decision needs.
 */
#define REST_REPEAT (-2)

struct rest {
    int                node;
    const struct rest* next;
};

struct run {
    struct regex* re;
    const char*   begin;        /* the whole line, for ^ */
    long          steps;
};

struct repeat_rest {
    struct rest        head;    /* first, so this can be used as a rest */
    int                node;
    int                count;   /* iterations done, including this one */
    const char*        from;    /* where this iteration started */
    const struct rest* after;   /* what follows the whole repeat */
};

static int match_node(struct run* r, int node, const char* at,
                      const struct rest* rest, const char** end);
static int match_repeat(struct run* r, int node, int count, const char* at,
                        const struct rest* rest, const char** end);

static int match_rest(struct run* r, const struct rest* rest, const char* at,
                      const char** end)
{
    if (rest == 0) {
        *end = at;
        return 1;
    }
    if (rest->node == REST_REPEAT) {
        const struct repeat_rest* link = (const struct repeat_rest*)rest;
        /* An iteration that consumed nothing must not be repeated: `(a*)*`
         * against "b" would otherwise go round forever, each time matching the
         * empty string and each time deciding to try again. One empty
         * iteration is enough to satisfy a minimum; a second is not progress.
         */
        if (at == link->from)
            return match_rest(r, link->after, at, end);
        return match_repeat(r, link->node, link->count, at, link->after, end);
    }
    return match_node(r, rest->node, at, rest->next, end);
}

static char fold(const struct run* r, char c)
{
    if (!r->re->ignore_case)
        return c;
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int match_node(struct run* r, int node, const char* at,
                      const struct rest* rest, const char** end)
{
    if (node < 0)
        return match_rest(r, rest, at, end);
    if (++r->steps > MAX_STEPS)
        return 0;               /* give up rather than run forever */

    const struct node* n = &r->re->nodes[node];
    switch (n->kind) {
    case N_EMPTY:
        return match_rest(r, rest, at, end);

    case N_CHAR:
        if (*at == '\0' || fold(r, *at) != fold(r, n->ch))
            return 0;
        return match_rest(r, rest, at + 1, end);

    case N_ANY:
        /* Not a newline, as everywhere: these match within a line, and the
         * callers here hand over one line at a time anyway. */
        if (*at == '\0' || *at == '\n')
            return 0;
        return match_rest(r, rest, at + 1, end);

    case N_SET: {
        if (*at == '\0')
            return 0;
        const unsigned char c = (unsigned char)*at;
        const int in = (n->set[c >> 3] >> (c & 7)) & 1;
        if (in == (n->negate ? 1 : 0))
            return 0;
        return match_rest(r, rest, at + 1, end);
    }

    case N_BEGIN:
        if (at != r->begin)
            return 0;
        return match_rest(r, rest, at, end);

    case N_END:
        if (*at != '\0')
            return 0;
        return match_rest(r, rest, at, end);

    case N_CONCAT: {
        /* The right side becomes part of what follows the left. */
        const struct rest link = { n->right, rest };
        return match_node(r, n->left, at, &link, end);
    }

    case N_ALT:
        if (match_node(r, n->left, at, rest, end))
            return 1;
        return match_node(r, n->right, at, rest, end);

    case N_REPEAT:
        return match_repeat(r, node, 0, at, rest, end);
    }
    return 0;
}

static int match_repeat(struct run* r, int node, int count, const char* at,
                        const struct rest* rest, const char** end)
{
    const struct node* n = &r->re->nodes[node];
    if (++r->steps > MAX_STEPS)
        return 0;

    /* Greedy: another iteration is tried before settling for what is already
     * matched. The continuation comes back here with the count raised, so the
     * recursion is over iterations rather than over a loop counter. */
    if (n->max < 0 || count < n->max) {
        struct repeat_rest link;
        link.head.node = REST_REPEAT;
        link.head.next = 0;
        link.node  = node;
        link.count = count + 1;
        link.from  = at;
        link.after = rest;

        const char* stop = 0;
        if (match_node(r, n->left, at, &link.head, &stop)) {
            *end = stop;
            return 1;
        }
    }
    /* Enough of them already, so the rest of the pattern gets its turn. */
    if (count >= n->min)
        return match_rest(r, rest, at, end);
    return 0;
}

/* --- the two entry points ------------------------------------------------ */

struct regex* regex_compile(const char* pattern, int ignore_case,
                            const char** error)
{
    struct regex* re = (struct regex*)malloc(sizeof(struct regex));
    if (re == 0) {
        if (error != 0)
            *error = "out of memory";
        return 0;
    }
    re->count = 0;
    re->root = -1;
    re->ignore_case = ignore_case;
    re->steps = 0;

    struct parser p;
    p.re = re;
    p.at = pattern;
    p.error = 0;

    re->root = parse_alternation(&p);
    if (re->root >= 0 && *p.at != '\0' && p.error == 0)
        p.error = *p.at == ')' ? "unmatched )" : "trailing rubbish";
    if (re->root < 0 || p.error != 0) {
        if (error != 0)
            *error = p.error != 0 ? p.error : "the pattern makes no sense";
        free(re);
        return 0;
    }
    return re;
}

void regex_free(struct regex* re)
{
    free(re);
}

int regex_search(struct regex* re, const char* text, int* start, int* end)
{
    struct run r;
    r.re = re;
    r.begin = text;

    /* Leftmost match: every starting position is tried in order, and the first
     * that matches wins. Not leftmost-longest - the alternation takes the
     * first branch that works, which is what a backtracker gives and what
     * every other backtracking engine does. */
    for (const char* at = text; ; ++at) {
        r.steps = 0;
        const char* stop = 0;
        if (match_node(&r, re->root, at, 0, &stop)) {
            if (start != 0) *start = (int)(at - text);
            if (end != 0)   *end = (int)(stop - text);
            return 1;
        }
        if (*at == '\0')
            break;
    }
    return 0;
}
