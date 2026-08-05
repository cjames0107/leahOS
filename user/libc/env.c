/* The environment.
 *
 * A vector of "NAME=value" strings, ending in a null pointer, handed over on
 * the stack when a program starts and passed on when it starts another. There
 * was none: execve took an envp argument and ignored it, and crt0 computed a
 * pointer that landed on the argument strings because no second vector had
 * ever been laid out for it to find.
 *
 * The vector starts as the kernel's, on the stack, which cannot be extended.
 * The first change copies it into memory of our own and grows from there -
 * which is why `environ` is a pointer that can move, and why nothing should
 * hold on to it across a setenv.
 */

#include <stdlib.h>
#include <string.h>

char** environ;

#define MAX_VARS 128

static char*  g_own[MAX_VARS + 1];
static int    g_count;
static int    g_owned;          /* environ points at g_own rather than the stack */

/* Does `entry` begin with "name=" ? */
static int names(const char* entry, const char* name, unsigned long len)
{
    return strncmp(entry, name, len) == 0 && entry[len] == '=';
}

char* getenv(const char* name)
{
    if (name == 0 || environ == 0)
        return 0;
    const unsigned long len = strlen(name);
    if (len == 0)
        return 0;
    for (char** e = environ; *e != 0; ++e)
        if (names(*e, name, len))
            return *e + len + 1;
    return 0;
}

/* Take a copy we can change. Until something calls setenv, the vector is the
 * kernel's and is left exactly as it was. */
static int take_ownership(void)
{
    if (g_owned)
        return 0;
    g_count = 0;
    if (environ != 0)
        for (char** e = environ; *e != 0 && g_count < MAX_VARS; ++e)
            g_own[g_count++] = *e;
    g_own[g_count] = 0;
    environ = g_own;
    g_owned = 1;
    return 0;
}

int setenv(const char* name, const char* value, int overwrite)
{
    if (name == 0 || name[0] == '\0' || value == 0)
        return -1;
    /* An '=' in the name would make a variable nothing could look up: the
     * lookup splits on the first one, so "a=b" set to "c" reads back as "a"
     * being "b=c". */
    for (const char* p = name; *p != '\0'; ++p)
        if (*p == '=')
            return -1;

    take_ownership();
    const unsigned long len = strlen(name);

    for (int i = 0; i < g_count; ++i) {
        if (!names(g_own[i], name, len))
            continue;
        if (!overwrite)
            return 0;
        char* entry = (char*)malloc(len + strlen(value) + 2);
        if (entry == 0)
            return -1;
        memcpy(entry, name, len);
        entry[len] = '=';
        strcpy(entry + len + 1, value);
        /* The old entry is not freed: free is a no-op in this libc, and
         * pretending otherwise would read as reclamation that does not
         * happen. */
        g_own[i] = entry;
        return 0;
    }

    if (g_count >= MAX_VARS)
        return -1;
    char* entry = (char*)malloc(len + strlen(value) + 2);
    if (entry == 0)
        return -1;
    memcpy(entry, name, len);
    entry[len] = '=';
    strcpy(entry + len + 1, value);
    g_own[g_count++] = entry;
    g_own[g_count] = 0;
    return 0;
}

int unsetenv(const char* name)
{
    if (name == 0 || name[0] == '\0')
        return -1;
    take_ownership();
    const unsigned long len = strlen(name);
    for (int i = 0; i < g_count; ++i) {
        if (!names(g_own[i], name, len))
            continue;
        /* Shuffle down rather than leaving a hole: the vector is walked to its
         * null terminator by everything that reads it, including the kernel. */
        for (int k = i; k < g_count; ++k)
            g_own[k] = g_own[k + 1];
        --g_count;
        return 0;
    }
    return 0;                   /* not there is not a failure */
}

int putenv(char* entry)
{
    /* Takes the caller's string as it stands, which is what putenv has always
     * done and why it is the one to avoid: the caller now owns a string the
     * environment points at. */
    if (entry == 0)
        return -1;
    const char* eq = strchr(entry, '=');
    if (eq == 0)
        return unsetenv(entry);
    take_ownership();
    const unsigned long len = (unsigned long)(eq - entry);
    for (int i = 0; i < g_count; ++i)
        if (names(g_own[i], entry, len)) {
            g_own[i] = entry;
            return 0;
        }
    if (g_count >= MAX_VARS)
        return -1;
    g_own[g_count++] = entry;
    g_own[g_count] = 0;
    return 0;
}

int clearenv(void)
{
    take_ownership();
    g_count = 0;
    g_own[0] = 0;
    return 0;
}
