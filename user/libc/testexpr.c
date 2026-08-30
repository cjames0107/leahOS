/* See <testexpr.h>.
 *
 * A recursive descent parser over the argument list, which is what the grammar
 * asks for once -a, -o and parentheses are in it:
 *
 *     expr   := term  ( -o term )*
 *     term   := factor ( -a factor )*
 *     factor := ! factor | ( expr ) | unary | binary | string
 *
 * The awkward part of test is not the operators, it is that its arguments are
 * ordinary words: "(" is a string that happens to look like a bracket, and
 * `[ -f = -f ]` compares two strings rather than asking about a file twice.
 * POSIX resolves that by counting arguments before parsing them, and so does
 * this: with three arguments, a binary operator in the middle wins over
 * anything else it could be read as.
 */

#include <string.h>
#include <sys/stat.h>
#include <testexpr.h>
#include <unistd.h>

#define TRUE_  0
#define FALSE_ 1
#define BAD_   2

struct scan {
    char* const* argv;
    int argc;
    int at;
    int bad;                    /* the expression did not parse */
};

static const char* peek(struct scan* s)
{
    return s->at < s->argc ? s->argv[s->at] : 0;
}

static int is(const char* word, const char* what)
{
    return word != 0 && strcmp(word, what) == 0;
}

/* A signed decimal, and whether it was one at all. test's integer operators
 * are an error on a word that is not a number rather than a comparison against
 * zero - `[ x -eq 0 ]` is a broken test, not a true one. */
static int number(const char* text, long* out)
{
    if (text == 0 || text[0] == '\0')
        return 0;
    int i = 0, sign = 1;
    if (text[0] == '-' || text[0] == '+') {
        sign = text[0] == '-' ? -1 : 1;
        i = 1;
        if (text[1] == '\0')
            return 0;
    }
    long value = 0;
    for (; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return 0;
        value = value * 10 + (text[i] - '0');
    }
    *out = sign * value;
    return 1;
}

/* The one-argument-and-a-file operators. Returns -1 when `op` is not one. */
static int file_test(const char* op, const char* path)
{
    if (op == 0 || op[0] != '-' || op[1] == '\0' || op[2] != '\0')
        return -1;

    /* -h and -L ask about the link itself, so they are the one pair that must
     * not follow it. Everything else is about what the name leads to. */
    struct stat st;
    const int is_link = op[1] == 'h' || op[1] == 'L';
    const int found = is_link ? (lstat(path, &st) == 0) : (stat(path, &st) == 0);

    switch (op[1]) {
    case 'e': return found ? TRUE_ : FALSE_;
    case 'f': return (found && st.st_type == S_IFREG) ? TRUE_ : FALSE_;
    case 'd': return (found && st.st_type == S_IFDIR) ? TRUE_ : FALSE_;
    case 'h':
    case 'L': return (found && st.st_type == S_IFLNK) ? TRUE_ : FALSE_;
    case 'p': return (found && st.st_type == S_IFIFO) ? TRUE_ : FALSE_;
    case 'b': return (found && st.st_type == S_IFBLK) ? TRUE_ : FALSE_;
    case 'c': return (found && st.st_type == S_IFCHR) ? TRUE_ : FALSE_;
    case 's': return (found && st.st_size > 0) ? TRUE_ : FALSE_;
    /* Readable, writable, executable - asked of this process, which is what
     * the question means. Root may read and write anything, and may execute
     * whatever has any execute bit at all. */
    case 'r':
    case 'w':
    case 'x': {
        if (!found)
            return FALSE_;
        const unsigned uid = getuid();
        unsigned bits = st.st_mode & 0777;
        unsigned want;
        if (op[1] == 'r')      want = 4;
        else if (op[1] == 'w') want = 2;
        else                   want = 1;
        if (uid == 0)
            return (op[1] != 'x' || (bits & 0111) != 0) ? TRUE_ : FALSE_;
        unsigned shift = (st.st_uid == uid) ? 6 : ((st.st_gid == getgid()) ? 3 : 0);
        return ((bits >> shift) & want) != 0 ? TRUE_ : FALSE_;
    }
    case 'z':
    case 'n':
        return -1;                      /* string tests, handled by the caller */
    default:
        return -1;
    }
}

static int parse_expr(struct scan* s);

static int parse_factor(struct scan* s)
{
    const char* word = peek(s);
    if (word == 0) {
        s->bad = 1;
        return FALSE_;
    }

    if (is(word, "!")) {
        ++s->at;
        const int inner = parse_factor(s);
        if (s->bad)
            return FALSE_;
        return inner == TRUE_ ? FALSE_ : TRUE_;
    }

    if (is(word, "(")) {
        ++s->at;
        const int inner = parse_expr(s);
        if (!is(peek(s), ")")) {
            s->bad = 1;
            return FALSE_;
        }
        ++s->at;
        return inner;
    }

    /* A binary operator two words along wins over anything the first word
     * could be on its own, which is what makes `[ -f = -f ]` a comparison of
     * two strings rather than a question about a file. */
    if (s->at + 2 < s->argc) {
        const char* op = s->argv[s->at + 1];
        const char* lhs = s->argv[s->at];
        const char* rhs = s->argv[s->at + 2];
        int handled = 1, result = FALSE_;
        long a = 0, b = 0;

        if (is(op, "=") || is(op, "==")) result = strcmp(lhs, rhs) == 0 ? TRUE_ : FALSE_;
        else if (is(op, "!="))          result = strcmp(lhs, rhs) != 0 ? TRUE_ : FALSE_;
        else if (is(op, "-eq") || is(op, "-ne") || is(op, "-lt") ||
                 is(op, "-le") || is(op, "-gt") || is(op, "-ge")) {
            if (!number(lhs, &a) || !number(rhs, &b)) {
                s->bad = 1;
                return FALSE_;
            }
            if (is(op, "-eq"))      result = a == b ? TRUE_ : FALSE_;
            else if (is(op, "-ne")) result = a != b ? TRUE_ : FALSE_;
            else if (is(op, "-lt")) result = a <  b ? TRUE_ : FALSE_;
            else if (is(op, "-le")) result = a <= b ? TRUE_ : FALSE_;
            else if (is(op, "-gt")) result = a >  b ? TRUE_ : FALSE_;
            else                    result = a >= b ? TRUE_ : FALSE_;
        }
        else if (is(op, "-nt") || is(op, "-ot")) {
            /* Newer and older. A file that is not there is older than one that
             * is, which is what makes `[ out -nt in ]` mean "needs building". */
            struct stat x, y;
            const int hx = stat(lhs, &x) == 0, hy = stat(rhs, &y) == 0;
            if (!hx && !hy)      result = FALSE_;
            else if (!hy)        result = is(op, "-nt") ? TRUE_ : FALSE_;
            else if (!hx)        result = is(op, "-nt") ? FALSE_ : TRUE_;
            else if (is(op, "-nt")) result = x.st_mtime > y.st_mtime ? TRUE_ : FALSE_;
            else                    result = x.st_mtime < y.st_mtime ? TRUE_ : FALSE_;
        }
        else handled = 0;

        if (handled) {
            s->at += 3;
            return result;
        }
    }

    /* A unary operator and its argument. */
    if (word[0] == '-' && word[1] != '\0' && word[2] == '\0' &&
        s->at + 1 < s->argc) {
        const char* arg = s->argv[s->at + 1];
        if (word[1] == 'z' || word[1] == 'n') {
            s->at += 2;
            const int empty = arg[0] == '\0';
            return (word[1] == 'z') == (empty != 0) ? TRUE_ : FALSE_;
        }
        const int answer = file_test(word, arg);
        if (answer >= 0) {
            s->at += 2;
            return answer;
        }
    }

    /* A bare word: true when it is not empty. This is last, so every operator
     * has already had its chance to claim it. */
    ++s->at;
    return word[0] != '\0' ? TRUE_ : FALSE_;
}

static int parse_term(struct scan* s)
{
    int left = parse_factor(s);
    while (!s->bad && is(peek(s), "-a")) {
        ++s->at;
        const int right = parse_factor(s);
        left = (left == TRUE_ && right == TRUE_) ? TRUE_ : FALSE_;
    }
    return left;
}

static int parse_expr(struct scan* s)
{
    int left = parse_term(s);
    while (!s->bad && is(peek(s), "-o")) {
        ++s->at;
        const int right = parse_term(s);
        left = (left == TRUE_ || right == TRUE_) ? TRUE_ : FALSE_;
    }
    return left;
}

int test_expr(int argc, char* const argv[])
{
    /* No arguments at all is false, which is what `[ ]` means and is not an
     * error - a shell expanding an empty variable into a test gets here often.
     * One argument is the bare-word case and needs no parser. */
    if (argc <= 0)
        return FALSE_;
    if (argc == 1)
        return argv[0][0] != '\0' ? TRUE_ : FALSE_;

    struct scan s = { argv, argc, 0, 0 };
    const int answer = parse_expr(&s);
    if (s.bad || s.at != argc)
        return BAD_;                    /* malformed, which is not false */
    return answer;
}
