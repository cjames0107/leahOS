/* leahOS shell.
 *
 * A read-eval loop: read a line, tokenise it, handle a couple of builtins, and
 * otherwise fork and exec /BIN/<COMMAND>.ELF. It also does the two things that
 * make a shell feel like a shell - redirection (<, >, >>) and a pipe (|) - by
 * wiring up file descriptors with dup2 before the exec. Operators must be
 * surrounded by spaces (ls / | cat, echo hi > f), which keeps the tokeniser
 * trivial.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TOKENS 64

static int tokenize(char* line, char** tokens)
{
    int n = 0;
    char* p = line;
    while (*p != '\0' && n < MAX_TOKENS - 1) {
        while (*p == ' ')
            *p++ = '\0';
        if (*p == '\0')
            break;
        tokens[n++] = p;
        while (*p != '\0' && *p != ' ')
            ++p;
    }
    tokens[n] = 0;
    return n;
}

static int read_line(char* buffer, int max)
{
    // One read for the whole line: the console driver cooks it - echoing keys
    // and applying backspace - and returns at the newline, so line editing lives
    // in one place rather than being re-implemented here.
    int n = (int)read(0, buffer, max - 1);
    if (n <= 0)
        return -1;
    if (buffer[n - 1] == '\n')
        --n;
    buffer[n] = '\0';
    return n;
}

/* /BIN/<COMMAND>.ELF, upper-cased for FAT's 8.3 names. */
static void command_path(const char* name, char* out)
{
    const char* prefix = "/BIN/";
    int i = 0;
    while (prefix[i] != '\0') {
        out[i] = prefix[i];
        ++i;
    }
    for (const char* c = name; *c != '\0' && i < 120; ++c) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
        out[i++] = ch;
    }
    const char* suffix = ".ELF";
    for (int j = 0; suffix[j] != '\0'; ++j)
        out[i++] = suffix[j];
    out[i] = '\0';
}

/* Pull the redirection operators out of a token range, leaving a clean argv. */
static void parse(char** tokens, int start, int end, char** argv,
                  char** infile, char** outfile, int* append)
{
    int argc = 0;
    *infile = 0;
    *outfile = 0;
    *append = 0;
    for (int i = start; i < end; ++i) {
        if (strcmp(tokens[i], "<") == 0 && i + 1 < end) {
            *infile = tokens[++i];
        } else if (strcmp(tokens[i], ">") == 0 && i + 1 < end) {
            *outfile = tokens[++i];
            *append = 0;
        } else if (strcmp(tokens[i], ">>") == 0 && i + 1 < end) {
            *outfile = tokens[++i];
            *append = 1;
        } else {
            argv[argc++] = tokens[i];
        }
    }
    argv[argc] = 0;
}

/* In the child: apply any redirections, then become the program. */
static _Noreturn void child(char** argv, char* infile, char* outfile, int append)
{
    if (infile != 0) {
        const int fd = open(infile, O_RDONLY);
        if (fd < 0) {
            printf("%s: cannot open\n", infile);
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (outfile != 0) {
        const int fd = open(outfile, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            printf("%s: cannot create\n", outfile);
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    char path[128];
    command_path(argv[0], path);
    execve(path, argv, 0);
    printf("%s: command not found\n", argv[0]);
    exit(127);
}

int main(void)
{
    printf("\nleahOS shell - try: ls /, cat /README.MD, ls / | cat, echo hi > /F\n");
    printf("builtins: cd, exit, help. put spaces around | < > >>\n");

    char line[256];
    char* tokens[MAX_TOKENS];

    for (;;) {
        char cwd[128];
        getcwd(cwd, sizeof(cwd));
        printf("%s $ ", cwd);

        if (read_line(line, sizeof(line)) < 0)
            break;

        const int n = tokenize(line, tokens);
        if (n == 0)
            continue;

        if (strcmp(tokens[0], "exit") == 0)
            break;
        if (strcmp(tokens[0], "help") == 0) {
            printf("builtins: cd exit help. commands run from /BIN.\n");
            printf("redirection: < > >>, pipe: |  (spaces around operators)\n");
            continue;
        }
        if (strcmp(tokens[0], "cd") == 0) {
            const char* dir = n > 1 ? tokens[1] : "/";
            if (chdir(dir) < 0)
                printf("cd: %s: no such directory\n", dir);
            continue;
        }

        int bar = -1;
        for (int i = 0; i < n; ++i) {
            if (strcmp(tokens[i], "|") == 0) {
                bar = i;
                break;
            }
        }

        if (bar >= 0) {
            char *largv[MAX_TOKENS], *lin, *lout;
            char *rargv[MAX_TOKENS], *rin, *rout;
            int lap, rap;
            parse(tokens, 0, bar, largv, &lin, &lout, &lap);
            parse(tokens, bar + 1, n, rargv, &rin, &rout, &rap);

            int pfd[2];
            if (pipe(pfd) < 0) {
                printf("pipe failed\n");
                continue;
            }
            if (fork() == 0) {
                dup2(pfd[1], 1);
                close(pfd[0]);
                close(pfd[1]);
                child(largv, lin, 0, 0);
            }
            if (fork() == 0) {
                dup2(pfd[0], 0);
                close(pfd[0]);
                close(pfd[1]);
                child(rargv, 0, rout, rap);
            }
            close(pfd[0]);
            close(pfd[1]);
            int status;
            wait(&status);
            wait(&status);
        } else {
            char* argv[MAX_TOKENS];
            char *in, *out;
            int append;
            parse(tokens, 0, n, argv, &in, &out, &append);
            if (argv[0] == 0)
                continue;
            if (fork() == 0)
                child(argv, in, out, append);
            int status;
            wait(&status);
        }
    }

    printf("shell exiting\n");
    return 0;
}
