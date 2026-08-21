#include <fcntl.h>
#include <prefs.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_KEYS 32
#define KEY_LEN  32
#define VAL_LEN  160

static char g_key[MAX_KEYS][KEY_LEN];
static char g_val[MAX_KEYS][VAL_LEN];
static int  g_n;
static int  g_loaded;

/* Which set of settings is being read and written. Empty until somebody says,
 * and then it is whatever they said. */
static char g_scope[32];
static int  g_dirty;

static void home_of(char* out, unsigned max)
{
    char name[64] = "";
    username(getuid(), name);
    /* root's home is /root; everyone else lives under /home. Matching what the
     * account database actually lays out matters more than being uniform. */
    if (strcmp(name, "root") == 0)
        snprintf(out, max, "/root");
    else
        snprintf(out, max, "/home/%s", name);
}

static void path_of(char* out, unsigned max)
{
    char home[96];
    home_of(home, sizeof(home));
    if (g_scope[0] == '\0')
        snprintf(g_scope, sizeof(g_scope), "%s", PREFS_DESKTOP);
    snprintf(out, max, "%s/.config/%s.conf", home, g_scope);
}

void prefs_scope(const char* name)
{
    if (name == 0 || name[0] == '\0')
        return;
    if (strcmp(g_scope, name) == 0)
        return;
    /* What was set in the old scope belongs to the old scope, so it goes back
     * before the file underneath these keys changes. */
    if (g_loaded && g_dirty)
        prefs_save();
    snprintf(g_scope, sizeof(g_scope), "%s", name);
    g_n = 0;
    g_loaded = 0;
    prefs_load();
}

static int find(const char* key)
{
    for (int i = 0; i < g_n; ++i)
        if (strcmp(g_key[i], key) == 0)
            return i;
    return -1;
}

static void set(const char* key, const char* value)
{
    g_dirty = 1;
    int i = find(key);
    if (i < 0) {
        if (g_n >= MAX_KEYS)
            return;
        i = g_n++;
        int k = 0;
        while (key[k] && k < KEY_LEN - 1) { g_key[i][k] = key[k]; ++k; }
        g_key[i][k] = '\0';
    }
    int k = 0;
    while (value[k] && k < VAL_LEN - 1) { g_val[i][k] = value[k]; ++k; }
    g_val[i][k] = '\0';
}

void prefs_load(void)
{
    g_loaded = 1;
    g_n = 0;
    char path[128];
    path_of(path, sizeof(path));
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;                     /* no file yet is not an error */
    static char buf[4096];
    const int len = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
        return;
    buf[len] = '\0';

    int i = 0;
    while (i < len) {
        char key[KEY_LEN], val[VAL_LEN];
        int k = 0;
        while (i < len && buf[i] != ' ' && buf[i] != '\n' && k < KEY_LEN - 1)
            key[k++] = buf[i++];
        key[k] = '\0';
        while (i < len && buf[i] == ' ') ++i;
        int v = 0;
        while (i < len && buf[i] != '\n' && v < VAL_LEN - 1)
            val[v++] = buf[i++];
        val[v] = '\0';
        while (i < len && buf[i] == '\n') ++i;
        if (key[0] != '\0' && key[0] != '#')
            set(key, val);
    }
}

int prefs_save(void)
{
    char path[128];
    path_of(path, sizeof(path));

    /* The directory, because nothing else makes it. A save that fails because
     * ~/.config is not there is a preference that silently does not stick, and
     * the only sign is the setting being back where it was next time. */
    char dir[112];
    home_of(dir, sizeof(dir));
    snprintf(&dir[strlen(dir)], sizeof(dir) - strlen(dir), "/.config");
    mkdir(dir);

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    g_dirty = 0;
    static char out[4096];
    int n = 0;
    /* Every key that was read is written back, including ones this program does
     * not understand - otherwise an older build would quietly drop a newer
     * one's settings. */
    for (int i = 0; i < g_n && n < (int)sizeof(out) - VAL_LEN - KEY_LEN - 4; ++i)
        n += snprintf(&out[n], sizeof(out) - (unsigned)n, "%s %s\n",
                      g_key[i], g_val[i]);
    const int wrote = (int)write(fd, out, (unsigned long)n);
    close(fd);
    return wrote == n ? 0 : -1;
}

unsigned prefs_get_u32(const char* key, unsigned fallback)
{
    if (!g_loaded) prefs_load();
    const int i = find(key);
    if (i < 0)
        return fallback;
    /* Hex, because everything stored this way so far is a colour. */
    unsigned v = 0;
    const char* p = g_val[i];
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (*p == '\0')
        return fallback;
    for (; *p; ++p) {
        unsigned d;
        if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A' + 10);
        else return fallback;
        v = v * 16 + d;
    }
    return v;
}

void prefs_set_u32(const char* key, unsigned value)
{
    if (!g_loaded) prefs_load();
    char v[24];
    snprintf(v, sizeof(v), "0x%06x", value);
    set(key, v);
}

const char* prefs_get_str(const char* key, const char* fallback)
{
    if (!g_loaded) prefs_load();
    const int i = find(key);
    return i < 0 ? fallback : g_val[i];
}

void prefs_set_str(const char* key, const char* value)
{
    if (!g_loaded) prefs_load();
    set(key, value);
}
