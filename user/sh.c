/* leahOS shell.
 *
 * Reads a line, splits it into words, handles a couple of builtins, and for
 * anything else forks and execs /BIN/<COMMAND>.ELF, waiting for it to finish -
 * the read-eval loop every UNIX shell is built on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_line(char* buffer, int max)
{
    int n = 0;
    while (n < max - 1) {
        char c;
        const long r = read(0, &c, 1);   /* the console echoes as we go */
        if (r <= 0)
            return -1;
        if (c == '\n')
            break;
        buffer[n++] = c;
    }
    buffer[n] = '\0';
    return n;
}

static int split(char* line, char** argv, int max)
{
    int argc = 0;
    char* p = line;
    while (*p != '\0' && argc < max - 1) {
        while (*p == ' ')
            *p++ = '\0';
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ')
            ++p;
    }
    argv[argc] = 0;
    return argc;
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

int main(void)
{
    printf("\nleahOS shell - try: ls /, cat /README.MD, echo hi, pwd, cd /DOCS\n");
    printf("builtins: cd, exit, help\n");

    char line[256];
    char* argv[32];

    for (;;) {
        char cwd[128];
        getcwd(cwd, sizeof(cwd));
        printf("%s $ ", cwd);

        if (read_line(line, sizeof(line)) < 0)
            break;

        const int argc = split(line, argv, 32);
        if (argc == 0)
            continue;

        if (strcmp(argv[0], "exit") == 0)
            break;
        if (strcmp(argv[0], "help") == 0) {
            printf("builtins: cd exit help. other commands run from /BIN.\n");
            continue;
        }
        if (strcmp(argv[0], "cd") == 0) {
            const char* dir = argc > 1 ? argv[1] : "/";
            if (chdir(dir) < 0)
                printf("cd: %s: no such directory\n", dir);
            continue;
        }

        char path[128];
        command_path(argv[0], path);

        const pid_t pid = fork();
        if (pid == 0) {
            execve(path, argv, 0);
            printf("%s: command not found\n", argv[0]);
            exit(127);
        }
        int status = 0;
        wait(&status);
    }

    printf("shell exiting\n");
    return 0;
}
