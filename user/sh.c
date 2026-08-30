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
 *   background     a trailing &, and jobs / fg / bg to go with it
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
#include <signal.h>
#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <testexpr.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_WORDS  128
#define MAX_LINE   1024
#define MAX_VARS   64
#define MAX_MATCH  256
#define MAX_JOBS   32

static int    g_status;         /* $? - what the last command returned */

/* Where a `break`, `continue` or `return` has to unwind to. Set by the builtin
 * and cleared by whichever construct is entitled to act on it, which is how a
 * `break` inside an `if` inside a loop leaves the loop and not the if. */
static int    g_break;          /* how many loops to leave */
static int    g_continue;
static int    g_returning;      /* a return, inside a function */

static const char* function_body(const char* name);
static char** g_args;           /* $1.. inside a script */
static int    g_argc;

/* --- jobs -----------------------------------------------------------------
 *
 * A job is a process group, and a pipeline is one job however many processes
 * it turns out to be: `a | b | c` is three programs that start together, are
 * interrupted together, and are finished when the last of them is. Putting
 * them in one group is what makes all three of those true at once, and is the
 * only reason the concept exists.
 *
 * Interactive only. A shell running a script has nobody to press Ctrl-Z at it
 * and no terminal to hand back and forth, and trying anyway would mean
 * tcsetpgrp failing on every command - see g_interactive.
 */
struct job {
    int  pgid;
    int  used;
    int  stopped;
    int  reported;              /* has the person been told it finished? */
    char text[128];             /* the line as typed, for `jobs` to show */
};

static struct job g_jobs[MAX_JOBS];
static int g_interactive;       /* a terminal to hand over, and a person */
static int g_shell_pgid;

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

/* --- command substitution ---------------------------------------------------
 *
 * $(...) and `...`: run the thing inside and stand in for what it printed.
 *
 * A child, because the command inside is a command like any other and may be a
 * pipeline, a loop or a function - so it needs a whole shell to run it, and
 * running it here would mean this shell's variables and redirections were
 * whatever the substitution left behind. The child writes to a pipe and this
 * end reads until it closes, which is also how it learns the child finished.
 *
 * Trailing newlines are stripped, all of them. That is not tidiness: `cd
 * $(pwd)` has to work, and every command that prints a line ends it. */
static int exec_text(char* text);

static void capture(const char* command, char* out, int max)
{
    out[0] = '\0';
    int fds[2];
    if (pipe(fds) != 0)
        return;

    __fd_before_fork();
    const int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        /* A copy of the command, because exec_text writes into what it is
         * given and this is the parent's string. */
        static char inner[MAX_LINE];
        snprintf(inner, sizeof(inner), "%s", command);
        exit(exec_text(inner));
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return;
    }

    int len = 0;
    for (;;) {
        const long n = read(fds[0], out + len, (unsigned long)(max - 1 - len));
        if (n <= 0)
            break;
        len += (int)n;
        if (len >= max - 1)
            break;
    }
    close(fds[0]);
    out[len] = '\0';
    waitpid(pid, 0, 0);

    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
}

/* The text inside $( ) or ` `, and where the closing delimiter was.
 *
 * Nesting is counted for $( ), because `$(echo $(date))` is ordinary and a
 * scan for the first ')' would cut it in the middle. Backticks do not nest -
 * there is nothing to count with, which is why $( ) replaced them. */
static int substitution(const char* p, char* out, int max, int* consumed)
{
    if (p[0] == '`') {
        int n = 0, i = 1;
        while (p[i] != '\0' && p[i] != '`') {
            if (p[i] == '\\' && p[i + 1] != '\0')
                ++i;
            if (n < max - 1)
                out[n++] = p[i];
            ++i;
        }
        out[n] = '\0';
        *consumed = p[i] == '`' ? i + 1 : i;
        return 1;
    }
    if (p[0] != '$' || p[1] != '(')
        return 0;

    int depth = 1, n = 0, i = 2, quote = 0;
    while (p[i] != '\0') {
        const char c = p[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (--depth == 0)
                break;
        }
        if (n < max - 1)
            out[n++] = c;
        ++i;
    }
    out[n] = '\0';
    *consumed = p[i] == ')' ? i + 1 : i;
    return 1;
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
    if (*p == '#') {
        /* How many there are, not counting the name - which is what $# means
         * everywhere and what `while [ $# -gt 0 ]` is counting down. */
        snprintf(answer, sizeof(answer), "%d", g_argc > 0 ? g_argc - 1 : 0);
        *at = p + 1;
        return answer;
    }
    if (*p == '@' || *p == '*') {
        int len = 0;
        answer[0] = '\0';
        for (int i = 1; i < g_argc && g_args != 0; ++i) {
            const int n = snprintf(&answer[len], sizeof(answer) - (unsigned long)len,
                                   "%s%s", len > 0 ? " " : "", g_args[i]);
            if (n < 0 || len + n >= (int)sizeof(answer))
                break;
            len += n;
        }
        *at = p + 1;
        return answer;
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
                } else if (*p == '$' || *p == '`') {
                    /* Inside double quotes a substitution still runs; what
                     * quoting suppresses is the splitting and globbing of what
                     * comes back, and both of those happen later. */
                    char inner[MAX_LINE], got[MAX_LINE];
                    int used = 0;
                    if (substitution(p, inner, sizeof(inner), &used)) {
                        capture(inner, got, sizeof(got));
                        for (const char* v = got; *v != '\0'; ++v)
                            word_add(&w, *v);
                        p += used;
                    } else if (*p == '`') {
                        word_add(&w, *p++);
                    } else {
                        for (const char* v = expand_one(&p); *v != '\0'; ++v)
                            word_add(&w, *v);
                    }
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
        } else if (*p == '$' || *p == '`') {
            char inner[MAX_LINE], got[MAX_LINE];
            int used = 0;
            if (substitution(p, inner, sizeof(inner), &used)) {
                capture(inner, got, sizeof(got));
                /* What comes back is words, not one word: `for f in $(ls)`
                 * has to see a file per turn. Whitespace splits it, which is
                 * what an unquoted substitution means everywhere. */
                for (const char* v = got; *v != '\0'; ++v) {
                    if (*v == ' ' || *v == '\t' || *v == '\n') {
                        flush_word(&w, &building, words, &count, max);
                        building = 0;
                        continue;
                    }
                    building = 1;
                    if (*v == '*' || *v == '?')
                        w.glob = 1;
                    word_add(&w, *v);
                }
                p += used;
            } else if (*p == '`') {
                word_add(&w, *p++);
            } else {
                /* An unquoted expansion can itself contain a pattern, so the
                 * glob flag comes from what came out rather than what went
                 * in. */
                for (const char* v = expand_one(&p); *v != '\0'; ++v) {
                    if (*v == '*' || *v == '?')
                        w.glob = 1;
                    word_add(&w, *v);
                }
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

/* --- jobs ------------------------------------------------------------------ */

static int job_slot(int pgid)
{
    for (int i = 0; i < MAX_JOBS; ++i)
        if (g_jobs[i].used && g_jobs[i].pgid == pgid)
            return i;
    return -1;
}

/* Remember a job, or find the one already remembered. The number a person sees
 * is the slot plus one, which is why nothing is ever compacted: %1 has to go on
 * meaning the same job for as long as it exists. */
static int job_add(int pgid, const char* text, int stopped)
{
    int at = job_slot(pgid);
    if (at < 0)
        for (at = 0; at < MAX_JOBS && g_jobs[at].used; ++at)
            ;
    if (at >= MAX_JOBS)
        return 0;
    g_jobs[at].used     = 1;
    g_jobs[at].pgid     = pgid;
    g_jobs[at].stopped  = stopped;
    g_jobs[at].reported = 0;
    snprintf(g_jobs[at].text, sizeof(g_jobs[at].text), "%s", text);
    return at + 1;
}

static void job_forget(int pgid)
{
    const int at = job_slot(pgid);
    if (at >= 0)
        g_jobs[at].used = 0;
}

/* Which job `%n`, `%%` or a bare number means. Zero for none. */
static int job_named(const char* word)
{
    if (word == 0) {
        /* No name: the most recent stopped job, then the most recent of any.
         * `fg` on its own is the common case and should not need one. */
        for (int i = MAX_JOBS - 1; i >= 0; --i)
            if (g_jobs[i].used && g_jobs[i].stopped)
                return g_jobs[i].pgid;
        for (int i = MAX_JOBS - 1; i >= 0; --i)
            if (g_jobs[i].used)
                return g_jobs[i].pgid;
        return 0;
    }
    if (word[0] == '%')
        ++word;
    if (word[0] == '%' || word[0] == '+' || word[0] == '\0')
        return job_named(0);
    {
        const int n = atoi_simple(word);
        if (n >= 1 && n <= MAX_JOBS && g_jobs[n - 1].used)
            return g_jobs[n - 1].pgid;
    }
    return 0;
}

/* Anything that finished while we were not looking. Called before each prompt,
 * which is where every shell reports this: interrupting a person mid-command
 * to say a background job ended is worse than telling them a moment later. */
static void job_reap_finished(void)
{
    for (;;) {
        int status = 0;
        const int pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0)
            break;
        const int pgid = (int)getpgid(pid);
        const int at = job_slot(pgid > 0 ? pgid : pid);
        if (at < 0)
            continue;
        if (WIFSTOPPED(status)) {
            g_jobs[at].stopped = 1;
            printf("[%d] stopped   %s\n", at + 1, g_jobs[at].text);
        } else if (WIFCONTINUED(status)) {
            g_jobs[at].stopped = 0;
        } else {
            /* One process of a pipeline finishing is not the job finishing.
             * Ask the kernel whether any of the group is left. */
            if (kill(-g_jobs[at].pgid, 0) == 0)
                continue;
            printf("[%d] done      %s\n", at + 1, g_jobs[at].text);
            g_jobs[at].used = 0;
        }
    }
}

/* Wait for a job that has the terminal.
 *
 * The terminal is handed over before and taken back after, and taken back
 * whichever way the job ends - finished, killed, or stopped and still sitting
 * there. Forgetting the last of those is how a shell ends up typing into a
 * program that is no longer running.
 */
static int foreground(int pgid, int last_pid, int count, const char* text)
{
    int status = 0, left = count;

    /* A shell running a script has no job control to do: nobody to press
     * Ctrl-Z at it, no terminal to hand over, and no prompt to come back to.
     * It waits for its children the way it always did, and `pgid` is 0 to say
     * that no group was made for them. */
    const int which = pgid > 0 ? -pgid : -1;

    if (g_interactive)
        tcsetpgrp(0, pgid);

    while (left > 0) {
        int one = 0;
        const int got = waitpid(which, &one, g_interactive ? WUNTRACED : 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;       /* our own Ctrl-C; the job is still there */
            break;
        }
        if (WIFSTOPPED(one)) {
            /* A stop stops the whole job, so there is nothing more to wait
             * for - the rest of the pipeline is suspended too. */
            const int n = job_add(pgid, text, 1);
            if (g_interactive)
                tcsetpgrp(0, g_shell_pgid);
            printf("[%d] stopped   %s\n", n, text);
            return 128 + WSTOPSIG(one);
        }
        --left;
        /* A pipeline's status is its last stage's, which is why the others are
         * reaped and thrown away. */
        if (got == last_pid)
            status = one;
    }

    if (g_interactive)
        tcsetpgrp(0, g_shell_pgid);
    if (pgid > 0)
        job_forget(pgid);

    /* Said out loud, because a program that vanishes without a word looks like
     * a bug in the shell. Not for SIGINT: the person pressed the key and does
     * not need telling what it did. */
    if (WIFSIGNALED(status) && WTERMSIG(status) != SIGINT)
        printf("%s: %s\n", text, signal_name(WTERMSIG(status)));

    return WSHELL_STATUS(status);
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
static int run_simple(char words[][256], int start, int end, int background,
                      const char* text)
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

    /* The expression builtins. In here rather than only in /bin/test because
     * a loop that forks and execs once per comparison spends most of its time
     * doing that; the rules themselves are libc's, so the two cannot drift. */
    if (strcmp(argv[0], "test") == 0 || strcmp(argv[0], "[") == 0) {
        int n = 0;
        while (argv[n] != 0) ++n;
        n -= 1;                                 /* past the command name */
        if (strcmp(argv[0], "[") == 0) {
            if (n < 1 || strcmp(argv[n], "]") != 0) {
                fprintf(stderr, "[: missing ]\n");
                return 2;
            }
            --n;
        }
        return test_expr(n, &argv[1]);
    }
    if (strcmp(argv[0], "true") == 0 || strcmp(argv[0], ":") == 0)
        return 0;
    if (strcmp(argv[0], "false") == 0)
        return 1;

    /* Leaving a loop, or a function. These set a flag rather than jumping,
     * because what has to unwind is the statement walker and it is the one
     * entitled to decide how far. */
    if (strcmp(argv[0], "break") == 0) {
        g_break = argv[1] ? atoi_simple(argv[1]) : 1;
        if (g_break < 1) g_break = 1;
        return 0;
    }
    if (strcmp(argv[0], "continue") == 0) {
        g_continue = argv[1] ? atoi_simple(argv[1]) : 1;
        if (g_continue < 1) g_continue = 1;
        return 0;
    }
    if (strcmp(argv[0], "return") == 0) {
        g_returning = 1;
        return argv[1] ? atoi_simple(argv[1]) : g_status;
    }
    /* shift, which is how a function or a script walks its arguments. */
    if (strcmp(argv[0], "shift") == 0) {
        int by = argv[1] ? atoi_simple(argv[1]) : 1;
        if (by < 0) by = 0;
        if (g_args != 0 && by < g_argc) {
            for (int i = 1; i + by < g_argc; ++i)
                g_args[i] = g_args[i + by];
            g_argc -= by;
        } else if (g_args != 0) {
            g_argc = 1;
        }
        return 0;
    }

    /* A function this shell has been told about, which is looked for before
     * the filesystem: that is what lets a script define `log` and have every
     * later call mean its own. */
    {
        const char* body = function_body(argv[0]);
        if (body != 0) {
            /* The arguments are copied, not pointed at.
             *
             * argv points into the shared array that tokenize writes every
             * word into, and running the body tokenizes - so by the time the
             * body asked for $1 the caller's arguments had been overwritten
             * with the body's own words. `greet() { echo hello $1; }; greet
             * world` printed "hello hello", because "hello" had landed in the
             * slot "world" was read from. It looked right whenever the body's
             * second word happened not to be reached first. */
            #define FN_ARGS 16
            char store[FN_ARGS][256];
            char* mine[FN_ARGS + 1];
            int n = 0;
            while (argv[n] != 0 && n < FN_ARGS) {
                snprintf(store[n], sizeof(store[0]), "%s", argv[n]);
                mine[n] = store[n];
                ++n;
            }
            mine[n] = 0;

            char** saved = g_args;
            const int saved_n = g_argc;
            g_args = mine;
            g_argc = n;

            char run[4096];
            snprintf(run, sizeof(run), "%s", body);
            const int status = exec_text(run);

            g_args = saved;
            g_argc = saved_n;
            g_returning = 0;            /* a return leaves the function, not more */
            return status;
        }
    }

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
    if (strcmp(argv[0], "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; ++i)
            if (g_jobs[i].used)
                printf("[%d] %-9s %s\n", i + 1,
                       g_jobs[i].stopped ? "stopped" : "running",
                       g_jobs[i].text);
        return 0;
    }
    if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
        const int to_front = argv[0][0] == 'f';
        const int pgid = job_named(argv[1]);
        if (pgid == 0) {
            fprintf(stderr, "%s: no such job\n", argv[0]);
            return 1;
        }
        const int at = job_slot(pgid);
        char what[128];
        snprintf(what, sizeof(what), "%s", at >= 0 ? g_jobs[at].text : "job");

        /* Told to carry on either way; the difference between fg and bg is
         * only which of us then has the terminal and who waits. */
        if (at >= 0)
            g_jobs[at].stopped = 0;
        printf("%s\n", what);
        if (to_front) {
            kill(-pgid, SIGCONT);
            /* One process is waited for, not the whole pipeline: how many
             * stages it had is not recorded, and the group going away is what
             * actually ends the wait. */
            return foreground(pgid, pgid, 1, what);
        }
        kill(-pgid, SIGCONT);
        return 0;
    }
    if (strcmp(argv[0], ":") == 0 || strcmp(argv[0], "true") == 0)
        return 0;
    if (strcmp(argv[0], "false") == 0)
        return 1;
    if (strcmp(argv[0], "help") == 0) {
        printf("builtins: cd exit export unset source : true false jobs fg bg help\n");
        printf("quoting:  'literal' \"expanded\" \\c\n");
        printf("variables: $NAME ${NAME} $? $$   assignment: NAME=value\n");
        printf("patterns: * ?    lists: ; && ||\n");
        printf("redirection: < > >>   pipe: |   background: trailing &\n");
        printf("jobs: jobs, fg %%1, bg %%1;  Ctrl-C interrupts, Ctrl-Z suspends\n");
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
        cli_fail("cannot fork: %s", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        /* Its own group, before it is anything else. Done here as well as in
         * the parent because there is no saying which of the two runs first,
         * and the child must be in the group before the terminal is handed
         * over to it - a race whose standard answer is to do it on both sides
         * and let them agree. */
        if (g_interactive || background)
            setpgid(0, 0);
        child(argv, &redir);
    }
    const int pgid = (g_interactive || background) ? pid : 0;
    if (pgid > 0)
        setpgid(pid, pgid);

    if (background) {
        const int n = job_add(pgid, text, 0);
        printf("[%d] %d\n", n, pid);
        return 0;
    }
    return foreground(pgid, pid, 1, text);
}

/* A pipeline: commands joined by |, of any length. Its status is the last
 * stage's, which is what every shell reports. */
static int run_pipeline(char words[][256], int start, int end, int background,
                        const char* text)
{
    int bars[MAX_WORDS], count = 0;
    for (int i = start; i < end; ++i)
        if (strcmp(words[i], "|") == 0 && count < MAX_WORDS)
            bars[count++] = i;

    if (count == 0)
        return run_simple(words, start, end, background, text);

    int from = start, in_fd = -1, pgid = 0, last_pid = 0;
    for (int stage = 0; stage <= count; ++stage) {
        const int to = (stage < count) ? bars[stage] : end;
        int pfd[2] = { -1, -1 };
        if (stage < count && pipe(pfd) < 0) {
            cli_fail("cannot pipe: %s", strerror(errno));
            return 1;
        }

        const int pid = fork();
        if (pid == 0) {
            /* Every stage joins the first stage's group, so the pipeline is
             * one job: one Ctrl-C reaches all of it, and one wait covers all
             * of it. The first stage starts the group by naming itself. */
            if (g_interactive || background)
                setpgid(0, pgid);
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
        if (g_interactive || background) {
            if (pgid == 0)
                pgid = pid;
            setpgid(pid, pgid);
        }
        last_pid = pid;

        if (in_fd >= 0)
            close(in_fd);
        if (pfd[1] >= 0)
            close(pfd[1]);
        in_fd = pfd[0];
        from = to + 1;
    }
    if (in_fd >= 0)
        close(in_fd);

    if (background) {
        const int n = job_add(pgid, text, 0);
        printf("[%d] %d\n", n, last_pid);
        return 0;
    }
    /* Every stage is waited for. Leaving them to init means the prompt comes
     * back before the output has finished arriving. */
    return foreground(pgid, last_pid, count + 1, text);
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
                    status = run_pipeline(words, 0, end, background, segment);
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

/* --- compound commands ------------------------------------------------------
 *
 * if, while, until, for, case and functions.
 *
 * A shell without these is a launcher: it can run things and join them with
 * pipes, and it cannot decide anything. Everything a UNIX is held together
 * with - the script that sets a machine up, the one that builds it, the one
 * that checks whether it needs building - is a program in this language, and
 * without a conditional there is no language to write it in.
 *
 * The unit is the statement rather than the line, because `if x; then y; fi`
 * on one line and the same thing on five are the same program and it would be
 * absurd for one to work and not the other. So a script is split on `;` and
 * newline into a list of statements, and the keywords are read off the front
 * of them.
 *
 * Splitting happens before any expansion, and expansion still happens command
 * by command as each one is about to run - so `X=1; echo $X` works, which it
 * would not if the whole script were expanded up front.
 */

#define MAX_STMTS 512
#define MAX_FUNCS 32

static struct {
    char name[64];
    char* body;                 /* the text between { and }, kept */
} g_funcs[MAX_FUNCS];
static int g_func_count;

/* Split `text` into statements, in place.
 *
 * Quotes, backslashes and substitutions are stepped over rather than looked
 * inside: a `;` inside "..." or inside $( ) is not a separator, and a shell
 * that thought it was would cut `echo "a;b"` in half. */
/* Emit one statement, splitting a leading `do`, `then` or `else` off what
 * follows it.
 *
 * `do if [ x ]` becomes `do` and `if [ x ]`, so that every construct starts a
 * statement of its own. Without that, a construct opened in another
 * statement's tail is invisible to the depth counting - the scan looking for
 * the outer `fi` sees the inner one first and stops in the wrong place, which
 * is how `if a; then if b; then c; fi; fi` came to be read as an if with no
 * fi. Splitting once here is the alternative to teaching every scan and every
 * range about tails. */
static void emit(char* text, char** out, int* count, int max)
{
    for (;;) {
        if (*count >= max)
            return;
        char* p = text;
        while (*p == ' ' || *p == '\t') ++p;
        int n = 0;
        while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t')
            ++n;
        const int is_keyword =
            (n == 2 && strncmp(p, "do", 2) == 0) ||
            (n == 4 && strncmp(p, "then", 4) == 0) ||
            (n == 4 && strncmp(p, "else", 4) == 0);
        if (!is_keyword || p[n] == '\0') {
            out[(*count)++] = text;
            return;
        }
        p[n] = '\0';                    /* the keyword, on its own */
        out[(*count)++] = text;
        text = &p[n + 1];               /* and round again for the rest */
    }
}

static int split_statements(char* text, char** out, int max)
{
    int count = 0;
    char* start = text;
    char* p = text;
    int quote = 0, paren = 0;

    while (*p != '\0') {
        if (quote != 0) {
            if (*p == '\\' && quote == '"' && p[1] != '\0') ++p;
            else if (*p == quote) quote = 0;
            ++p;
            continue;
        }
        if (*p == '\\' && p[1] != '\0') { p += 2; continue; }
        if (*p == '\'' || *p == '"') { quote = *p; ++p; continue; }
        if (*p == '`') {
            /* Backticks are their own quote, and the only thing that ends one
             * is another backtick. */
            ++p;
            while (*p != '\0' && *p != '`') {
                if (*p == '\\' && p[1] != '\0') ++p;
                ++p;
            }
            if (*p == '`') ++p;
            continue;
        }
        if (*p == '$' && p[1] == '(') { paren += 1; p += 2; continue; }
        if (paren > 0) {
            if (*p == '(') ++paren;
            else if (*p == ')') --paren;
            ++p;
            continue;
        }
        if (*p == '#' && (p == text || p[-1] == ' ' || p[-1] == '\t' ||
                          p[-1] == '\n' || p[-1] == ';')) {
            /* A comment runs to the end of its line, not of the script. */
            while (*p != '\0' && *p != '\n')
                *p++ = ' ';
            continue;
        }

        /* ;; ends a case arm and is a statement of its own, so that the arm
         * before it can be found by looking for it. */
        if (*p == ';' && p[1] == ';') {
            *p = '\0';
            emit(start, out, &count, max);
            if (count < max) out[count++] = (char*)";;";
            p += 2;
            start = p;
            continue;
        }
        if (*p == ';' || *p == '\n') {
            *p = '\0';
            emit(start, out, &count, max);
            ++p;
            start = p;
            continue;
        }
        ++p;
    }
    emit(start, out, &count, max);

    /* Empty statements are what the separators leave behind - `a;; b` and a
     * blank line both make one - and nothing downstream wants them. ";;" is
     * kept because it means something. */
    int keep = 0;
    for (int i = 0; i < count; ++i) {
        const char* t = out[i];
        while (*t == ' ' || *t == '\t') ++t;
        if (*t != '\0')
            out[keep++] = out[i];
    }
    return keep;
}

/* The first word of a statement, for reading a keyword off it. */
static void first_word(const char* stmt, char* out, int max)
{
    while (*stmt == ' ' || *stmt == '\t') ++stmt;
    int n = 0;
    while (*stmt != '\0' && *stmt != ' ' && *stmt != '\t' && n < max - 1)
        out[n++] = *stmt++;
    out[n] = '\0';
}

static int word_is(const char* stmt, const char* what)
{
    char w[32];
    first_word(stmt, w, sizeof(w));
    return strcmp(w, what) == 0;
}

/* What is left of a statement after its first word. */
static const char* after_word(const char* stmt)
{
    while (*stmt == ' ' || *stmt == '\t') ++stmt;
    while (*stmt != '\0' && *stmt != ' ' && *stmt != '\t') ++stmt;
    while (*stmt == ' ' || *stmt == '\t') ++stmt;
    return stmt;
}

/* Whether a statement opens or closes a block, for finding the one that
 * matches. `do` is not counted: it belongs to the while or for that opened
 * already, and counting it would need `done` to close two. */
/* Where the command in a statement actually begins.
 *
 * A construct often shares a statement with the keyword before it: `do if [ x
 * ]` and `then while true` are ordinary. A depth count that read only the
 * first word would not see the `if` there, would not count it, and would then
 * match the outer construct against the inner one's `fi` - which is how a for
 * loop came to report that it had no `done`. */
static const char* command_start(const char* stmt)
{
    for (;;) {
        char w[16];
        first_word(stmt, w, sizeof(w));
        if (strcmp(w, "do") == 0 || strcmp(w, "then") == 0 ||
            strcmp(w, "else") == 0)
            stmt = after_word(stmt);
        else
            return stmt;
    }
}

static int opens_block(const char* stmt)
{
    const char* c = command_start(stmt);
    return word_is(c, "if") || word_is(c, "while") ||
           word_is(c, "until") || word_is(c, "for") ||
           word_is(c, "case");
}

static int closes_block(const char* stmt)
{
    const char* c = command_start(stmt);
    return word_is(c, "fi") || word_is(c, "done") ||
           word_is(c, "esac");
}

/* The statement, at this nesting level, whose first word is one of `wanted`.
 * Returns its index, or `to` when there is none. */
static int find_at_depth(char** stmt, int from, int to, const char* const* wanted)
{
    int depth = 0;
    for (int i = from; i < to; ++i) {
        if (depth == 0)
            for (int k = 0; wanted[k] != 0; ++k)
                if (word_is(stmt[i], wanted[k]))
                    return i;
        if (opens_block(stmt[i]))  ++depth;
        if (closes_block(stmt[i])) --depth;
    }
    return to;
}

static int exec_stmts(char** stmt, int from, int to);

/* The body that follows a keyword.
 *
 * `then echo yes` and `then` on its own with `echo yes` under it are the same
 * program, so a body is whatever is left on the keyword's own statement
 * followed by the statements after it. Missing the first half is what made
 * `if true; then echo taken; fi` print nothing while the same thing written
 * over three lines worked. */
static int exec_body(char** stmt, int head, int from, int to)
{
    char tail[MAX_LINE];
    snprintf(tail, sizeof(tail), "%s", after_word(stmt[head]));
    if (tail[0] == '\0')
        return to > from ? exec_stmts(stmt, from, to) : 0;

    /* The tail and the statements after it are one list, not two.
     *
     * `do if [ x ]` opens a construct that a `fi` two statements later closes,
     * and running the tail on its own would have that if looking for its fi
     * inside a list of one - which fails, and takes the loop with it. */
    char* joined[MAX_STMTS];
    int n = 0;
    joined[n++] = tail;
    for (int i = from; i < to && n < MAX_STMTS; ++i)
        joined[n++] = stmt[i];
    return exec_stmts(joined, 0, n);
}

/* A condition is a list of statements, and its answer is the last one's. The
 * first part of it rides on the keyword's own statement, as a body does. */
static int condition_true(char** stmt, int head, int from, int to)
{
    return exec_body(stmt, head, from, to) == 0;
}

static int do_if(char** stmt, int from, int to, int* status)
{
    static const char* const kThen[] = { "then", 0 };
    static const char* const kNext[] = { "elif", "else", "fi", 0 };
    static const char* const kFi[]   = { "fi", 0 };

    /* The whole construct, so an unfinished one is skipped rather than run
     * half way. */
    const int end = find_at_depth(stmt, from + 1, to, kFi);
    if (end >= to) {
        fprintf(stderr, "sh: if without fi\n");
        *status = 2;
        return to;
    }

    int at = from;
    for (;;) {
        /* `if cmd` and `elif cmd` carry the first part of the condition on
         * their own statement; the rest run up to `then`. */
        const int then_at = find_at_depth(stmt, at + 1, end, kThen);
        if (then_at >= end) {
            fprintf(stderr, "sh: if without then\n");
            *status = 2;
            return end + 1;
        }
        const int met = condition_true(stmt, at, at + 1, then_at);

        const int branch_end = find_at_depth(stmt, then_at + 1, end, kNext);
        if (met) {
            *status = exec_body(stmt, then_at, then_at + 1, branch_end);
            return end + 1;
        }
        if (branch_end >= end)
            break;                              /* nothing else to try */
        if (word_is(stmt[branch_end], "else")) {
            *status = exec_body(stmt, branch_end, branch_end + 1, end);
            return end + 1;
        }
        at = branch_end;                        /* an elif */
    }
    *status = 0;                                /* no branch ran: not a failure */
    return end + 1;
}

static int do_loop(char** stmt, int from, int to, int* status, int until)
{
    static const char* const kDo[]   = { "do", 0 };
    static const char* const kDone[] = { "done", 0 };

    const int end = find_at_depth(stmt, from + 1, to, kDone);
    const int do_at = find_at_depth(stmt, from + 1, to, kDo);
    if (end >= to || do_at >= end) {
        fprintf(stderr, "sh: %s without do ... done\n", until ? "until" : "while");
        *status = 2;
        return to;
    }

    *status = 0;
    for (int turn = 0; turn < 100000; ++turn) {
        int met = condition_true(stmt, from, from + 1, do_at);
        if (until)
            met = !met;
        if (!met)
            break;

        *status = exec_body(stmt, do_at, do_at + 1, end);
        if (g_returning)
            break;
        if (g_continue > 0 && --g_continue > 0) break;
        if (g_break > 0) { --g_break; break; }
    }
    return end + 1;
}

static int do_for(char** stmt, int from, int to, int* status)
{
    static const char* const kDo[]   = { "do", 0 };
    static const char* const kDone[] = { "done", 0 };

    const int end = find_at_depth(stmt, from + 1, to, kDone);
    const int do_at = find_at_depth(stmt, from + 1, to, kDo);
    if (end >= to || do_at >= end) {
        fprintf(stderr, "sh: for without do ... done\n");
        *status = 2;
        return to;
    }

    /* `for NAME in WORDS`. The words are expanded here and once: a list that
     * was re-globbed every turn would change under a loop that creates files. */
    char header[MAX_LINE];
    snprintf(header, sizeof(header), "%s", after_word(stmt[from]));
    char name[64];
    first_word(header, name, sizeof(name));
    const char* rest = after_word(header);
    if (word_is(rest, "in"))
        rest = after_word(rest);
    else if (rest[0] == '\0')
        rest = "\"$@\"";                        /* bare `for x` is the arguments */

    static char words[MAX_WORDS + 1][256];
    char list[MAX_LINE];
    snprintf(list, sizeof(list), "%s", rest);
    const int n = tokenize(list, words, MAX_WORDS);

    *status = 0;
    for (int i = 0; i < n; ++i) {
        var_set(name, words[i]);
        *status = exec_body(stmt, do_at, do_at + 1, end);
        if (g_returning)
            break;
        if (g_continue > 0 && --g_continue > 0) break;
        if (g_break > 0) { --g_break; break; }
    }
    return end + 1;
}

static int do_case(char** stmt, int from, int to, int* status)
{
    static const char* const kEsac[] = { "esac", 0 };
    const int end = find_at_depth(stmt, from + 1, to, kEsac);
    if (end >= to) {
        fprintf(stderr, "sh: case without esac\n");
        *status = 2;
        return to;
    }

    /* `case WORD in`, where the first arm often rides on the same statement:
     * `case $x in a) echo one;;` is how it is written more often than not. So
     * the header is split at the word `in`, the subject taken from the left of
     * it and the first arm from the right. */
    char header[MAX_LINE];
    snprintf(header, sizeof(header), "%s", after_word(stmt[from]));

    char subject[256] = "";
    const char* first_arm = "";
    {
        /* The word `in`, not the letters: a subject of "index" must not be
         * cut in half. */
        char* p = header;
        char* found = 0;
        while (*p != '\0') {
            if ((p == header || p[-1] == ' ' || p[-1] == '\t') &&
                p[0] == 'i' && p[1] == 'n' &&
                (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
                found = p;
                break;
            }
            ++p;
        }
        if (found != 0) {
            *found = '\0';
            first_arm = found + 2;
            while (*first_arm == ' ' || *first_arm == '\t')
                ++first_arm;
        }
        static char words[MAX_WORDS + 1][256];
        const int n = tokenize(header, words, MAX_WORDS);
        if (n > 0)
            snprintf(subject, sizeof(subject), "%s", words[0]);
    }

    *status = 0;
    /* The arms, in order. `at` is the statement an arm starts on; `text` is
     * the arm's own text, which for the first one comes off the header. */
    int at = from;
    char arm_text[MAX_LINE];
    snprintf(arm_text, sizeof(arm_text), "%s", first_arm);
    if (arm_text[0] == '\0') {
        at = from + 1;
        if (at < end)
            snprintf(arm_text, sizeof(arm_text), "%s", stmt[at]);
    }

    while (at < end) {
        const char* arm = arm_text;
        while (*arm == ' ' || *arm == '\t') ++arm;
        /* A leading ( is allowed and means nothing: `(a|b)` and `a|b)` are the
         * same arm. */
        if (*arm == '(') ++arm;
        const char* close = strchr(arm, ')');
        if (close == 0)
            break;                              /* not an arm; nothing to run */

        char patterns[256];
        int plen = (int)(close - arm);
        if (plen > (int)sizeof(patterns) - 1)
            plen = (int)sizeof(patterns) - 1;
        snprintf(patterns, sizeof(patterns), "%.*s", plen, arm);

        /* One arm may list several patterns, separated by |. */
        int hit = 0;
        char one[128];
        int n = 0;
        for (const char* q = patterns; ; ++q) {
            if (*q == '|' || *q == '\0') {
                one[n] = '\0';
                char* t = one;
                while (*t == ' ' || *t == '\t') ++t;
                int tl = (int)strlen(t);
                while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t'))
                    t[--tl] = '\0';
                if (t[0] != '\0' && match(t, subject))
                    hit = 1;
                n = 0;
                if (*q == '\0')
                    break;
                continue;
            }
            if (n < (int)sizeof(one) - 1)
                one[n++] = *q;
        }

        /* The body: whatever follows the ) here, then the statements up to the
         * ;; that ends the arm. */
        char body_head[MAX_LINE];
        snprintf(body_head, sizeof(body_head), "%s", close + 1);
        int arm_end = at + 1;
        while (arm_end < end && !word_is(stmt[arm_end], ";;"))
            ++arm_end;

        if (hit) {
            if (body_head[0] != '\0') {
                char* one_stmt[1] = { body_head };
                *status = exec_stmts(one_stmt, 0, 1);
            }
            if (arm_end > at + 1)
                *status = exec_stmts(stmt, at + 1, arm_end);
            return end + 1;
        }

        at = arm_end + 1;
        if (at < end)
            snprintf(arm_text, sizeof(arm_text), "%s", stmt[at]);
    }
    return end + 1;
}

/* `name() { ... }`, possibly spread over many statements. Returns the index
 * past the closing brace, or `from` when this is not a definition. */
static int do_function(char** stmt, int from, int to)
{
    const char* text = stmt[from];
    while (*text == ' ' || *text == '\t') ++text;

    /* The shape is a name, then (), then a brace - here or on a later
     * statement. `function name {` is the other spelling and is not accepted:
     * one way in is enough. */
    char name[64];
    int n = 0;
    while (text[n] != '\0' && n < (int)sizeof(name) - 1 &&
           (text[n] == '_' || (text[n] >= 'a' && text[n] <= 'z') ||
            (text[n] >= 'A' && text[n] <= 'Z') ||
            (text[n] >= '0' && text[n] <= '9')))
        ++n;
    if (n == 0)
        return from;
    memcpy(name, text, (unsigned long)n);
    name[n] = '\0';
    const char* p = text + n;
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] != '(' || p[1] != ')')
        return from;
    p += 2;
    while (*p == ' ' || *p == '\t') ++p;

    /* The body: from the opening brace to the matching close. Braces are
     * counted rather than searched for, so a function with an if in it ends
     * where it should. */
    int i = from;
    const char* head = p;
    if (head[0] != '{') {
        if (++i >= to)
            return from;
        head = stmt[i];
        while (*head == ' ' || *head == '\t') ++head;
        if (head[0] != '{')
            return from;
    }
    ++head;                                     /* past the { */

    static char body[4096];
    int len = 0;
    int depth = 1;
    const char* piece = head;
    for (;;) {
        for (const char* q = piece; *q != '\0'; ++q) {
            if (*q == '{') ++depth;
            else if (*q == '}') {
                if (--depth == 0) {
                    /* Everything before the closing brace belongs to the
                     * body. */
                    const int take = (int)(q - piece);
                    if (len + take < (int)sizeof(body) - 1) {
                        memcpy(&body[len], piece, (unsigned long)take);
                        len += take;
                    }
                    body[len] = '\0';
                    goto done;
                }
            }
        }
        {
            const int take = (int)strlen(piece);
            if (len + take + 1 < (int)sizeof(body) - 1) {
                memcpy(&body[len], piece, (unsigned long)take);
                len += take;
                body[len++] = '\n';
            }
        }
        if (++i >= to) {
            fprintf(stderr, "sh: %s: no closing }\n", name);
            return to;
        }
        piece = stmt[i];
    }
done:
    {
        int slot = -1;
        for (int k = 0; k < g_func_count; ++k)
            if (strcmp(g_funcs[k].name, name) == 0)
                slot = k;
        if (slot < 0 && g_func_count < MAX_FUNCS)
            slot = g_func_count++;
        if (slot >= 0) {
            snprintf(g_funcs[slot].name, sizeof(g_funcs[slot].name), "%s", name);
            free(g_funcs[slot].body);
            g_funcs[slot].body = (char*)malloc((unsigned long)len + 1);
            if (g_funcs[slot].body != 0)
                memcpy(g_funcs[slot].body, body, (unsigned long)len + 1);
        }
    }
    return i + 1;
}

static const char* function_body(const char* name)
{
    for (int i = 0; i < g_func_count; ++i)
        if (strcmp(g_funcs[i].name, name) == 0)
            return g_funcs[i].body;
    return 0;
}

/* Run a list of statements, reading the keywords off the front of them. */
static int exec_stmts(char** stmt, int from, int to)
{
    int status = g_status;
    int i = from;
    while (i < to) {
        if (g_returning || g_break > 0 || g_continue > 0)
            break;
        const char* one = stmt[i];
        while (*one == ' ' || *one == '\t') ++one;
        if (*one == '\0') { ++i; continue; }

        if (word_is(one, "if"))         { i = do_if(stmt, i, to, &status); continue; }
        if (word_is(one, "while"))      { i = do_loop(stmt, i, to, &status, 0); continue; }
        if (word_is(one, "until"))      { i = do_loop(stmt, i, to, &status, 1); continue; }
        if (word_is(one, "for"))        { i = do_for(stmt, i, to, &status); continue; }
        if (word_is(one, "case"))       { i = do_case(stmt, i, to, &status); continue; }
        /* A keyword that got here has no construct around it - `fi` on its
         * own, or a `then` this shell has already stepped over. Skipped
         * quietly: complaining about it would mean complaining about every
         * well-formed script too, because these are how the constructs above
         * find their ends. */
        if (word_is(one, "then") || word_is(one, "else") || word_is(one, "elif") ||
            word_is(one, "fi") || word_is(one, "do") || word_is(one, "done") ||
            word_is(one, "esac") || word_is(one, ";;")) { ++i; continue; }

        const int after = do_function(stmt, i, to);
        if (after != i) { i = after; continue; }

        char line[MAX_LINE];
        snprintf(line, sizeof(line), "%s", stmt[i]);
        status = run_line(line);
        g_status = status;
        ++i;
    }
    return status;
}

/* Whether what has been typed so far is a program or the start of one.
 *
 * Counted rather than parsed: every construct here is opened by one word and
 * closed by another, so the question is whether they balance. A `then` with no
 * `fi` after it leaves the count above zero and the shell asks for more.
 * Written into a copy, because counting means splitting and splitting writes
 * into what it is given. */
static int needs_more(const char* text)
{
    static char copy[MAX_LINE * 8];
    static char* stmts[MAX_STMTS];
    snprintf(copy, sizeof(copy), "%s", text);
    const int n = split_statements(copy, stmts, MAX_STMTS);

    int depth = 0;
    for (int i = 0; i < n; ++i) {
        if (opens_block(stmts[i])) ++depth;
        if (closes_block(stmts[i])) --depth;
    }
    return depth > 0;
}

/* A whole script, or the inside of a substitution. Writes into `text`. */
static int exec_text(char* text)
{
    static char* stmts[MAX_STMTS];
    char** slot = stmts;
    /* Nested: a substitution runs a shell inside this one, and a `for` body
     * that contains a substitution would otherwise share the array being
     * walked. The nesting is shallow, so one spare set is enough to notice
     * when it is not. */
    static int depth;
    char* local[MAX_STMTS];
    if (depth++ > 0)
        slot = local;
    const int n = split_statements(text, slot, MAX_STMTS);
    const int status = exec_stmts(slot, 0, n);
    --depth;
    return status;
}

/* --- scripts -------------------------------------------------------------------- */

/* The whole file at once, and then split.
 *
 * It used to read a line and run it, which cannot work now: `if` and its `fi`
 * are on different lines and a shell that has forgotten the first by the time
 * it reaches the second has no way to connect them. So the script is read
 * whole - which also means a syntax error is found before anything has run
 * rather than half way through. */
static int run_script(const char* path, int argc, char** argv)
{
    FILE* in = fopen(path, "r");
    if (in == 0) {
        cli_fail("%s: %s", path, strerror(errno));
        return 127;
    }
    static char text[65536];
    int len = 0;
    int c;
    while ((c = fgetc(in)) != EOF && len < (int)sizeof(text) - 1)
        text[len++] = (char)c;
    text[len] = '\0';
    const int truncated = (c != EOF);
    fclose(in);
    if (truncated)
        cli_fail("%s: only the first %d bytes were read", path,
                 (int)sizeof(text) - 1);

    char** saved_args = g_args;
    const int saved_argc = g_argc;
    g_args = argv;
    g_argc = argc;

    const int status = exec_text(text);

    g_args = saved_args;
    g_argc = saved_argc;
    g_returning = 0;
    return status;
}

int main(int argc, char** argv)
{
    /* Everything after the script or the -c string belongs to what is being
     * run, not to this shell, so the library parses none of it. */
    cli_begin(argc, argv, "[-c command | script [arg...]]", 0);

    /* A script named on the command line, or -c and a string. Either way this
     * shell is not interactive and prints no prompt - a prompt written into a
     * pipe is noise in whatever reads it. */
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        g_args = &argv[2];
        g_argc = argc - 2;
        /* exec_text, not run_line: `sh -c 'for f in *; do ...; done'` is one
         * argument holding a whole program, and it has to be read as one. */
        static char text[MAX_LINE * 4];
        snprintf(text, sizeof(text), "%s", argv[2]);
        return exec_text(text);
    }
    if (argc > 1)
        return run_script(argv[1], argc - 1, &argv[1]);

    /* Interactive from here: there is a terminal to hand back and forth and a
     * person to press keys at it. Everything above returns before this, which
     * is deliberate - a shell running a script has no job control to do, and
     * tcsetpgrp would fail on every command it ran. */
    g_interactive = tty_fd() >= 0;
    if (g_interactive) {
        /* The keys the terminal turns into signals go to the foreground job.
         * When there is no job they come here instead, and a shell that took
         * the default action on them would exit on its own Ctrl-C. Children
         * are unaffected: execve puts every disposition back to default. */
        signal(SIGINT,  SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);

        /* term calls setsid before exec, so this shell already leads its own
         * session and group; this is just finding out what it is called. */
        g_shell_pgid = (int)getpgrp();
        tcsetpgrp(0, g_shell_pgid);
    }

    /* The user's own startup file, before the first prompt.
     *
     * This is where a PATH, a variable or an alias that should be there in
     * every shell actually goes; without it there was nowhere to put one, and
     * the environment a session had was whatever login compiled in. Only for
     * an interactive shell: a script gets the environment it was given, and
     * one that behaved differently because of a file in somebody's home
     * directory would be a script that cannot be relied on.
     *
     * A missing file is the normal case and says nothing. */
    {
        const char* home = getenv("HOME");
        if (home != 0 && home[0] != '\0') {
            char profile[256];
            snprintf(profile, sizeof(profile), "%s/.profile", home);
            struct stat st;
            if (stat(profile, &st) == 0)
                run_script(profile, 0, 0);
        }
    }

    printf("leahOS shell - try: ls /, cat /usr/share/doc/readme.md, ls | wc\n");
    printf("builtins: cd exit export unset source help. `help` lists the rest.\n");

    char line[MAX_LINE];
    static char pending[MAX_LINE * 8], run[MAX_LINE * 8];
    for (;;) {
        char cwd[128];

        /* Before the prompt, not during a command: a background job finishing
         * is worth saying, and saying it in the middle of somebody's typing is
         * worse than saying it a moment later. */
        job_reap_finished();

        getcwd(cwd, sizeof(cwd));
        printf("%s $ ", cwd);
        fflush(stdout);

        /* One read for the whole line: the console driver cooks it - echoing
         * keys and applying backspace - and returns at the newline, so line
         * editing lives in one place rather than being re-implemented here.
         *
         * A signal cutting the read short is not the end of the input. Ctrl-C
         * at a prompt arrives here as an interruption, and a shell that took
         * it for end-of-file would close its own terminal - which is exactly
         * what it did until read learned to say which had happened. */
        errno = 0;
        int n = (int)read(0, line, sizeof(line) - 1);
        while (n < 0 && errno == EINTR) {
            printf("\n%s $ ", cwd);
            fflush(stdout);
            errno = 0;
            n = (int)read(0, line, sizeof(line) - 1);
        }
        if (n <= 0)
            break;
        line[(n > 0 && line[n - 1] == '\n') ? n - 1 : n] = '\0';

        /* A construct spread over several lines is read until it closes.
         *
         * `if` and its `fi` are two lines and running the first on its own
         * would be running half a program, so the shell keeps reading with a
         * different prompt until the blocks balance - which is what every
         * shell does and what makes typing a loop at a prompt possible. */
        snprintf(pending, sizeof(pending), "%s", line);
        while (needs_more(pending)) {
            printf("> ");
            fflush(stdout);
            errno = 0;
            int more = (int)read(0, line, sizeof(line) - 1);
            while (more < 0 && errno == EINTR) {
                errno = 0;
                more = (int)read(0, line, sizeof(line) - 1);
            }
            if (more <= 0)
                break;
            line[(more > 0 && line[more - 1] == '\n') ? more - 1 : more] = '\0';
            const int at = (int)strlen(pending);
            snprintf(&pending[at], sizeof(pending) - (unsigned long)at,
                     "\n%s", line);
        }
        snprintf(run, sizeof(run), "%s", pending);
        exec_text(run);
    }

    printf("shell exiting\n");
    return g_status;
}
