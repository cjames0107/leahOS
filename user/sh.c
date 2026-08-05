/* leahOS shell.
 *
 * Read a line, work out what it says, run it. What makes that more than a loop
 * is everything between reading and running:
 *
 *   quoting        'literal' and "expanded", and \ before one character
 *   variables      $NAME, ${NAME}, $?, $$, and $1.. inside a script
 *   assignment     NAME=value, and export to put one in the environment
 *   patterns       * and ? matched against the filesystem
 *   lists          ; runs both, && runs the second if the first worked,
 *                  || runs it if the first did not
 *   redirection    < > >>
 *   pipes          |, any number of stages
 *   background     a trailing &
 *   scripts        sh FILE, sh -c, and #! which libc's execve honours
 *
 * Operators no longer need spaces around them. They did, because the tokeniser
 * split on spaces and nothing else, so `ls>f` was a single word.
 *
 * The order of expansion is the usual one and it matters: variables first,
 * then patterns. That is why $F where F holds "*.c" matches files, and why a
 * filename with a space in it survives being quoted.
 */

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_WORDS  128
#define MAX_LINE   1024
#define MAX_VARS   64
#define MAX_MATCH  256

static int    g_status;         /* $? - what the last command returned */
static char** g_args;           /* $1.. inside a script */
static int    g_argc;

/* --- shell variables ----------------------------------------------------------
 *
 * Separate from the environment: a shell variable is this shell's, and only
 * becomes a child's when it is exported. That distinction is the whole reason
 * both exist.
 */

static struct { char name[64]; char value[256]; } g_vars[MAX_VARS];
static int g_var_count;

static const char* var_get(const char* name)
{
    for (int i = 0; i < g_var_count; ++i)
        if (strcmp(g_vars[i].name, name) == 0)
            return g_vars[i].value;
    return getenv(name);
}

static void var_set(const char* name, const char* value)
{
    /* Already exported? Then setting it again should change what a child
     * sees, not shadow it with a shell variable nobody passes on. */
    if (getenv(name) != 0) {
        setenv(name, value, 1);
        return;
    }
    for (int i = 0; i < g_var_count; ++i)
        if (strcmp(g_vars[i].name, name) == 0) {
            snprintf(g_vars[i].value, sizeof(g_vars[i].value), "%s", value);
            return;
        }
    if (g_var_count >= MAX_VARS)
        return;
    snprintf(g_vars[g_var_count].name, sizeof(g_vars[0].name), "%s", name);
    snprintf(g_vars[g_var_count].value, sizeof(g_vars[0].value), "%s", value);
    ++g_var_count;
}

/* --- words --------------------------------------------------------------------
 *
 * A word is built a character at a time, because with quoting the characters
 * that end a word depend on what came before them.
 */

struct word {
    char text[512];
    int  len;
    int  quoted;        /* had quotes, so patterns in it are literal */
    int  glob;          /* has an unquoted * or ? worth expanding */
};

static void word_add(struct word* w, char c)
{
    if (w->len < (int)sizeof(w->text) - 1)
        w->text[w->len++] = c;
}

/* $NAME, ${NAME}, $?, $$, $1.. - the text it stands for. */
static const char* expand_one(const char** at)
{
    static char answer[256];
    const char* p = *at + 1;            /* past the $ */

    if (*p == '?') {
        snprintf(answer, sizeof(answer), "%d", g_status);
        *at = p + 1;
        return answer;
    }
    if (*p == '$') {
        snprintf(answer, sizeof(answer), "%d", getpid());
        *at = p + 1;
        return answer;
    }
    if (*p >= '0' && *p <= '9') {
        const int which = *p - '0';
        *at = p + 1;
        if (g_args != 0 && which < g_argc)
            return g_args[which];
        return "";
    }

    char name[64];
    int n = 0;
    const int braced = (*p == '{');
    if (braced)
        ++p;
    while (*p != '\0' && n < (int)sizeof(name) - 1 &&
           (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9')))
        name[n++] = *p++;
    name[n] = '\0';
    if (braced && *p == '}')
        ++p;
    *at = p;

    if (n == 0)
        return "$";                     /* a lone $ is just a dollar sign */
    const char* value = var_get(name);
    return value ? value : "";          /* unset reads as empty, as it should */
}

/* --- patterns ------------------------------------------------------------------ */

static int match(const char* pattern, const char* text)
{
    for (;;) {
        if (*pattern == '\0')
            return *text == '\0';
        if (*pattern == '*') {
            ++pattern;
            for (const char* t = text;; ++t) {
                if (match(pattern, t))
                    return 1;
                if (*t == '\0')
                    return 0;
            }
        }
        if (*text == '\0')
            return 0;
        if (*pattern != '?' && *pattern != *text)
            return 0;
        ++pattern;
        ++text;
    }
}

/* Expand one pattern into `out`, returning how many names it produced. Zero
 * means nothing matched, and the caller then uses the pattern as written -
 * which is what every Bourne shell does, and why `ls *.nothing` complains
 * about a file called "*.nothing" rather than silently listing everything. */
static int expand_pattern(const char* pattern, char out[][256], int max)
{
    /* Only the last component is matched. A pattern spanning directories would
     * need the walk to fan out at every level, which is a different piece of
     * work; the directory part here is taken literally. */
    const char* slash = strrchr(pattern, '/');
    char dir[256];
    const char* leaf;
    if (slash != 0) {
        const int len = (int)(slash - pattern);
        if (len > 0)
            snprintf(dir, sizeof(dir), "%.*s", len, pattern);
        else
            snprintf(dir, sizeof(dir), "/");
        leaf = slash + 1;
    } else {
        snprintf(dir, sizeof(dir), ".");
        leaf = pattern;
    }

    static struct dirent entries[MAX_MATCH];
    const int n = getdents(dir, entries, MAX_MATCH);
    if (n <= 0)
        return 0;

    int found = 0;
    for (int i = 0; i < n && found < max; ++i) {
        const char* name = entries[i].d_name;
        if (name[0] == '.' && leaf[0] != '.')
            continue;                   /* hidden unless asked for by name */
        if (!match(leaf, name))
            continue;
        if (slash != 0)
            snprintf(out[found], 256, "%s/%s",
                     strcmp(dir, "/") == 0 ? "" : dir, name);
        else
            snprintf(out[found], 256, "%s", name);
        ++found;
    }

    /* Sorted, so `*` gives the same order every time and a loop over it is
     * repeatable. Insertion sort: this is a directory listing, not a database. */
    for (int i = 1; i < found; ++i) {
        char hold[256];
        snprintf(hold, sizeof(hold), "%s", out[i]);
        int k = i - 1;
        while (k >= 0 && strcmp(out[k], hold) > 0) {
            snprintf(out[k + 1], 256, "%s", out[k]);
            --k;
        }
        snprintf(out[k + 1], 256, "%s", hold);
    }
    return found;
}

/* --- tokenising ----------------------------------------------------------------
 *
 * Straight to words, with quoting and variables resolved on the way. Operators
 * come out as words of their own, so the parser above finds them without
 * caring how they were spaced.
 */

static void flush_word(struct word* w, int* building, char words[][256],
                       int* count, int max)
{
    if (!*building)
        return;
    w->text[w->len] = '\0';

    if (w->glob && !w->quoted) {
        static char matches[MAX_MATCH][256];
        const int m = expand_pattern(w->text, matches, MAX_MATCH);
        if (m > 0) {
            for (int i = 0; i < m && *count < max; ++i)
                snprintf(words[(*count)++], 256, "%s", matches[i]);
        } else if (*count < max) {
            snprintf(words[(*count)++], 256, "%s", w->text);
        }
    } else if (*count < max) {
        snprintf(words[(*count)++], 256, "%s", w->text);
    }

    memset(w, 0, sizeof(*w));
    *building = 0;
}

static int tokenize(const char* line, char words[][256], int max)
{
    /* Longest first, so >> is not two > and 2>> is not "2" then ">>". The
     * numbered forms are here because a descriptor and its redirection are one
     * token with no space between them - `cmd 2>/dev/null` is universal, and
     * without this the 2 becomes an argument to cmd. */
    static const char* const kOps[] = { "2>>", "1>>", "2>", "1>", ">>",
                                        "&&", "||", ";", "|", "<", ">", "&" };
    struct word w;
    int count = 0, building = 0;
    const char* p = line;

    memset(&w, 0, sizeof(w));

    while (*p != '\0' && count < max) {
        if (*p == '#' && !building)
            break;                      /* a comment runs to the end of the line */

        if (*p == ' ' || *p == '\t') {
            flush_word(&w, &building, words, &count, max);
            ++p;
            continue;
        }

        /* Operators, longest first, so >> is not two > and && is not two
         * background markers. */
        int matched = 0;
        for (unsigned i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
            const int len = (int)strlen(kOps[i]);
            /* A numbered redirect only counts at the start of a word: in
             * `file2>x` the 2 belongs to the name, and only the > redirects. */
            if ((kOps[i][0] == '1' || kOps[i][0] == '2') && building)
                continue;
            if (strncmp(p, kOps[i], (size_t)len) == 0) {
                flush_word(&w, &building, words, &count, max);
                if (count < max)
                    snprintf(words[count++], 256, "%s", kOps[i]);
                p += len;
                matched = 1;
                break;
            }
        }
        if (matched)
            continue;

        building = 1;
        if (*p == '\'') {
            /* Single quotes: everything is itself, including $ and \. */
            w.quoted = 1;
            ++p;
            while (*p != '\0' && *p != '\'')
                word_add(&w, *p++);
            if (*p == '\'')
                ++p;
        } else if (*p == '"') {
            w.quoted = 1;
            ++p;
            while (*p != '\0' && *p != '"') {
                if (*p == '\\' && p[1] != '\0') {
                    ++p;
                    word_add(&w, *p++);
                } else if (*p == '$') {
                    for (const char* v = expand_one(&p); *v != '\0'; ++v)
                        word_add(&w, *v);
                } else {
                    word_add(&w, *p++);
                }
            }
            if (*p == '"')
                ++p;
        } else if (*p == '\\' && p[1] != '\0') {
            w.quoted = 1;
            ++p;
            word_add(&w, *p++);
        } else if (*p == '$') {
            /* An unquoted expansion can itself contain a pattern, so the glob
             * flag comes from what came out rather than what went in. */
            for (const char* v = expand_one(&p); *v != '\0'; ++v) {
                if (*v == '*' || *v == '?')
                    w.glob = 1;
                word_add(&w, *v);
            }
        } else {
            if (*p == '*' || *p == '?')
                w.glob = 1;
            word_add(&w, *p++);
        }
    }
    flush_word(&w, &building, words, &count, max);

    words[count][0] = '\0';
    return count;
}

/* --- running ------------------------------------------------------------------- */

/* Pull the redirections out of a word range, leaving a clean argv. */
struct redirect {
    char* in;
    char* out;      int out_append;
    char* err;      int err_append;
};

static void parse(char words[][256], int start, int end, char** argv,
                  struct redirect* r)
{
    int argc = 0;
    memset(r, 0, sizeof(*r));
    for (int i = start; i < end && argc < MAX_WORDS - 1; ++i) {
        const char* w = words[i];
        if (strcmp(w, "<") == 0 && i + 1 < end) {
            r->in = words[++i];
        } else if ((strcmp(w, ">") == 0 || strcmp(w, "1>") == 0) && i + 1 < end) {
            r->out = words[++i];
            r->out_append = 0;
        } else if ((strcmp(w, ">>") == 0 || strcmp(w, "1>>") == 0) && i + 1 < end) {
            r->out = words[++i];
            r->out_append = 1;
        } else if (strcmp(w, "2>") == 0 && i + 1 < end) {
            r->err = words[++i];
            r->err_append = 0;
        } else if (strcmp(w, "2>>") == 0 && i + 1 < end) {
            r->err = words[++i];
            r->err_append = 1;
        } else {
            argv[argc++] = words[i];
        }
    }
    argv[argc] = 0;
}

static _Noreturn void child(char** argv, const struct redirect* r)
{
    if (r->in != 0) {
        const int fd = open(r->in, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", r->in, strerror(errno));
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (r->out != 0) {
        const int fd = open(r->out, O_WRONLY | O_CREAT |
                            (r->out_append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", r->out, strerror(errno));
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    if (r->err != 0) {
        const int fd = open(r->err, O_WRONLY | O_CREAT |
                            (r->err_append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            /* Reported on the standard error it still has, which is the one
             * being redirected away - so this is the last chance to say it. */
            fprintf(stderr, "%s: %s\n", r->err, strerror(errno));
            exit(1);
        }
        dup2(fd, 2);
        close(fd);
    }
    char path[256];
    if (path_find_program(argv[0], path, sizeof(path)) != 0) {
        fprintf(stderr, "%s: command not found\n", argv[0]);
        exit(127);
    }
    execve(path, argv, environ);
    fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
    exit(126);
}

static int run_script(const char* path, int argc, char** argv);

/* One command with no operators left in it. Returns its exit status. */
static int run_simple(char words[][256], int start, int end, int background)
{
    char* argv[MAX_WORDS];
    struct redirect redir;
    parse(words, start, end, argv, &redir);
    if (argv[0] == 0)
        return 0;

    /* Builtins run in this process, which is the point of them: cd in a child
     * would change the child's directory and nothing else. */
    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "logout") == 0)
        exit(argv[1] ? atoi_simple(argv[1]) : g_status);

    if (strcmp(argv[0], "cd") == 0) {
        const char* home = getenv("HOME");
        const char* dir = argv[1] ? argv[1] : (home ? home : "/");
        if (chdir(dir) < 0) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) != 0)
            setenv("PWD", cwd, 1);
        return 0;
    }
    if (strcmp(argv[0], "export") == 0) {
        for (int i = 1; argv[i] != 0; ++i) {
            char* eq = strchr(argv[i], '=');
            if (eq != 0) {
                *eq = '\0';
                setenv(argv[i], eq + 1, 1);
            } else {
                /* Exporting a shell variable moves it, rather than leaving one
                 * of each with the same name to disagree later. */
                const char* value = var_get(argv[i]);
                setenv(argv[i], value ? value : "", 1);
                for (int k = 0; k < g_var_count; ++k)
                    if (strcmp(g_vars[k].name, argv[i]) == 0) {
                        g_vars[k] = g_vars[--g_var_count];
                        break;
                    }
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; argv[i] != 0; ++i) {
            unsetenv(argv[i]);
            for (int k = 0; k < g_var_count; ++k)
                if (strcmp(g_vars[k].name, argv[i]) == 0) {
                    g_vars[k] = g_vars[--g_var_count];
                    break;
                }
        }
        return 0;
    }
    if (strcmp(argv[0], ":") == 0 || strcmp(argv[0], "true") == 0)
        return 0;
    if (strcmp(argv[0], "false") == 0)
        return 1;
    if (strcmp(argv[0], "help") == 0) {
        printf("builtins: cd exit export unset source : true false help\n");
        printf("quoting:  'literal' \"expanded\" \\c\n");
        printf("variables: $NAME ${NAME} $? $$   assignment: NAME=value\n");
        printf("patterns: * ?    lists: ; && ||\n");
        printf("redirection: < > >>   pipe: |   background: trailing &\n");
        printf("commands are found along $PATH; #! scripts work\n");
        return 0;
    }
    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (argv[1] == 0)
            return 1;
        int n = 0;
        while (argv[n] != 0)
            ++n;
        return run_script(argv[1], n - 1, &argv[1]);
    }

    /* NAME=value on its own is an assignment, not a command. */
    {
        char* eq = strchr(argv[0], '=');
        if (eq != 0 && eq != argv[0] && argv[1] == 0) {
            *eq = '\0';
            var_set(argv[0], eq + 1);
            return 0;
        }
    }

    const int pid = fork();
    if (pid < 0) {
        fprintf(stderr, "sh: cannot fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0)
        child(argv, &redir);

    if (background) {
        /* Not reaped here. init is everyone's second parent and will collect
         * it; waiting is the one thing this must not do. */
        printf("[%d]\n", pid);
        return 0;
    }
    int status = 0;
    wait(&status);
    return status;
}

/* A pipeline: commands joined by |, of any length. Its status is the last
 * stage's, which is what every shell reports. */
static int run_pipeline(char words[][256], int start, int end, int background)
{
    int bars[MAX_WORDS], count = 0;
    for (int i = start; i < end; ++i)
        if (strcmp(words[i], "|") == 0 && count < MAX_WORDS)
            bars[count++] = i;

    if (count == 0)
        return run_simple(words, start, end, background);

    int from = start, in_fd = -1;
    for (int stage = 0; stage <= count; ++stage) {
        const int to = (stage < count) ? bars[stage] : end;
        int pfd[2] = { -1, -1 };
        if (stage < count && pipe(pfd) < 0) {
            fprintf(stderr, "sh: cannot pipe: %s\n", strerror(errno));
            return 1;
        }

        const int pid = fork();
        if (pid == 0) {
            if (in_fd >= 0)  { dup2(in_fd, 0);  close(in_fd); }
            if (pfd[1] >= 0) { dup2(pfd[1], 1); close(pfd[1]); }
            if (pfd[0] >= 0) close(pfd[0]);

            char* argv[MAX_WORDS];
            struct redirect redir;
            parse(words, from, to, argv, &redir);
            if (argv[0] == 0)
                exit(0);
            child(argv, &redir);
        }
        if (in_fd >= 0)
            close(in_fd);
        if (pfd[1] >= 0)
            close(pfd[1]);
        in_fd = pfd[0];
        from = to + 1;
    }
    if (in_fd >= 0)
        close(in_fd);

    /* Every stage is waited for. Leaving them to init means the prompt comes
     * back before the output has finished arriving. */
    int status = 0;
    for (int i = 0; i <= count; ++i) {
        int one = 0;
        if (wait(&one) < 0)
            break;
        status = one;
    }
    return status;
}

/* Where the next top-level ; && || starts, or -1. Quote-aware, because a
 * semicolon inside 'quotes' is a semicolon and not a separator. */
static int next_operator(const char* s, int* op_len)
{
    int quote = 0;                      /* the quote character we are inside */
    for (int i = 0; s[i] != '\0'; ++i) {
        if (quote != 0) {
            if (s[i] == '\\' && quote == '"' && s[i + 1] != '\0') ++i;
            else if (s[i] == quote) quote = 0;
            continue;
        }
        if (s[i] == '\'' || s[i] == '"') { quote = s[i]; continue; }
        if (s[i] == '\\' && s[i + 1] != '\0') { ++i; continue; }
        if (s[i] == '#') return -1;     /* the rest is a comment */
        if (s[i] == '&' && s[i + 1] == '&') { *op_len = 2; return i; }
        if (s[i] == '|' && s[i + 1] == '|') { *op_len = 2; return i; }
        if (s[i] == ';') { *op_len = 1; return i; }
    }
    return -1;
}

/* A whole line: pipelines joined by ; && ||.
 *
 * Split first, expand second. It used to tokenise the entire line up front,
 * which meant every variable in it was expanded before any of it had run - so
 * `X=hello; echo $X` printed nothing, because $X was looked up before the
 * assignment happened. A command's words are expanded when that command is
 * about to run, and not before.
 */
static int run_line(char* line)
{
    static char words[MAX_WORDS + 1][256];
    char segment[MAX_LINE];
    int status = g_status, skip = 0;
    const char* rest = line;

    for (;;) {
        int op_len = 0;
        const int cut = next_operator(rest, &op_len);
        const int len = (cut >= 0) ? cut : (int)strlen(rest);

        snprintf(segment, sizeof(segment), "%.*s", len, rest);

        if (!skip) {
            const int n = tokenize(segment, words, MAX_WORDS);
            if (n > 0) {
                int end = n, background = 0;
                if (strcmp(words[end - 1], "&") == 0) {
                    background = 1;
                    --end;
                }
                if (end > 0) {
                    status = run_pipeline(words, 0, end, background);
                    g_status = status;
                }
            }
        }

        if (cut < 0)
            break;
        /* What the operator means for what follows: && skips when this one
         * failed, || skips when it worked, ; never skips. */
        if (rest[cut] == '&')      skip = (status != 0);
        else if (rest[cut] == '|') skip = (status == 0);
        else                       skip = 0;
        rest += cut + op_len;
    }
    return status;
}

/* --- scripts -------------------------------------------------------------------- */

static int run_script(const char* path, int argc, char** argv)
{
    FILE* in = fopen(path, "r");
    if (in == 0) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
        return 127;
    }
    char** saved_args = g_args;
    const int saved_argc = g_argc;
    g_args = argv;
    g_argc = argc;

    char line[MAX_LINE];
    int status = 0;
    while (fgets(line, sizeof(line), in) != 0) {
        const int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        status = run_line(line);
    }
    fclose(in);

    g_args = saved_args;
    g_argc = saved_argc;
    return status;
}

int main(int argc, char** argv)
{
    /* A script named on the command line, or -c and a string. Either way this
     * shell is not interactive and prints no prompt - a prompt written into a
     * pipe is noise in whatever reads it. */
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        g_args = &argv[2];
        g_argc = argc - 2;
        char line[MAX_LINE];
        snprintf(line, sizeof(line), "%s", argv[2]);
        return run_line(line);
    }
    if (argc > 1)
        return run_script(argv[1], argc - 1, &argv[1]);

    printf("leahOS shell - try: ls /, cat /usr/share/doc/readme.md, ls | wc\n");
    printf("builtins: cd exit export unset source help. `help` lists the rest.\n");

    char line[MAX_LINE];
    for (;;) {
        char cwd[128];
        getcwd(cwd, sizeof(cwd));
        printf("%s $ ", cwd);
        fflush(stdout);

        /* One read for the whole line: the console driver cooks it - echoing
         * keys and applying backspace - and returns at the newline, so line
         * editing lives in one place rather than being re-implemented here. */
        const int n = (int)read(0, line, sizeof(line) - 1);
        if (n <= 0)
            break;
        line[(n > 0 && line[n - 1] == '\n') ? n - 1 : n] = '\0';
        run_line(line);
    }

    printf("shell exiting\n");
    return g_status;
}
