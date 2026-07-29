#include <bundle.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void upper_ext(const char* s, char* out, int max)
{
    int dot = -1;
    for (int i = 0; s[i] != '\0'; ++i)
        if (s[i] == '.') dot = i;
    int n = 0;
    if (dot >= 0)
        for (int i = dot; s[i] != '\0' && n < max - 1; ++i) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[n++] = c;
        }
    out[n] = '\0';
}

int bundle_is_app(const char* name)
{
    char ext[12];
    upper_ext(name, ext, sizeof(ext));
    return strcmp(ext, ".APP") == 0;
}

static void copy(char* dst, const char* src, int max)
{
    int n = 0;
    while (src[n] != '\0' && n < max - 1) { dst[n] = src[n]; ++n; }
    dst[n] = '\0';
}

int bundle_load(const char* path, struct bundle* out)
{
    memset(out, 0, sizeof(*out));
    copy(out->path, path, sizeof(out->path));

    char info[256];
    snprintf(info, sizeof(info), "%s/Info", path);
    const int fd = open(info, O_RDONLY);
    if (fd < 0)
        return -1;
    static char buf[2048];
    const int len = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
        return -1;
    buf[len] = '\0';

    int i = 0;
    while (i < len) {
        char key[16], val[128];
        int k = 0;
        while (i < len && buf[i] != ' ' && buf[i] != '\n' && k < 15)
            key[k++] = buf[i++];
        key[k] = '\0';
        while (i < len && buf[i] == ' ') ++i;
        int v = 0;
        while (i < len && buf[i] != '\n' && v < 127)
            val[v++] = buf[i++];
        val[v] = '\0';
        while (i < len && buf[i] == '\n') ++i;
        if (key[0] == '\0' || key[0] == '#')
            continue;

        if (strcmp(key, "name") == 0)      copy(out->name, val, sizeof(out->name));
        else if (strcmp(key, "exec") == 0) copy(out->exec, val, sizeof(out->exec));
        else if (strcmp(key, "icon") == 0) copy(out->icon, val, sizeof(out->icon));
        else if (strcmp(key, "menu") == 0) {
            if (out->menu_n < BUNDLE_MAX_MENU)
                copy(out->menu[out->menu_n++], val, 32);
        } else if (strcmp(key, "opens") == 0) {
            /* Space-separated, because a program that opens three kinds should
             * not need three lines to say so. */
            int at = 0;
            while (val[at] != '\0' && out->opens_n < BUNDLE_MAX_EXT) {
                while (val[at] == ' ') ++at;
                int n = 0;
                char one[12];
                while (val[at] != '\0' && val[at] != ' ' && n < 11) {
                    char c = val[at++];
                    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                    one[n++] = c;
                }
                one[n] = '\0';
                if (n > 0)
                    copy(out->opens[out->opens_n++], one, 12);
            }
        }
    }

    if (out->exec[0] == '\0')
        return -1;                  /* nothing to run is not an application */
    if (out->name[0] == '\0') {
        /* Fall back to the directory's own name without the extension, so a
         * bundle that forgot to name itself still reads sensibly. */
        int slash = -1;
        for (int k = 0; path[k] != '\0'; ++k)
            if (path[k] == '/') slash = k;
        copy(out->name, &path[slash + 1], sizeof(out->name));
        const int n = (int)strlen(out->name);
        if (n > 4) out->name[n - 4] = '\0';
    }
    return 0;
}

void bundle_exec(const struct bundle* b, char* out, int max)
{
    snprintf(out, (unsigned)max, "%s/%s", b->path, b->exec);
}

int bundle_for_document(const char* dir, const char* document,
                        struct bundle* out)
{
    char want[12];
    upper_ext(document, want, sizeof(want));
    if (want[0] == '\0')
        return -1;

    static struct dirent kids[64];
    const int n = getdents(dir, kids, 64);
    for (int i = 0; i < n; ++i) {
        if (!bundle_is_app(kids[i].d_name))
            continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, kids[i].d_name);
        struct bundle b;
        if (bundle_load(path, &b) != 0)
            continue;
        for (int k = 0; k < b.opens_n; ++k)
            if (strcmp(b.opens[k], want) == 0) {
                *out = b;
                return 0;
            }
    }
    return -1;
}

int bundle_find(const char* name, struct bundle* out)
{
    static struct dirent kids[64];
    const int n = getdents(BUNDLE_DIR, kids, 64);
    for (int i = 0; i < n; ++i) {
        if (!bundle_is_app(kids[i].d_name))
            continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", BUNDLE_DIR, kids[i].d_name);
        struct bundle b;
        if (bundle_load(path, &b) != 0)
            continue;
        /* By its declared name, or by the directory - either is what someone
         * would reasonably call it. */
        if (strcmp(b.name, name) == 0 || strcmp(kids[i].d_name, name) == 0) {
            *out = b;
            return 0;
        }
    }
    return -1;
}

int bundle_command(const char* name, char* out, int max)
{
    struct bundle b;
    if (bundle_find(name, &b) != 0) {
        out[0] = '\0';
        return -1;
    }
    bundle_exec(&b, out, max);
    return 0;
}

const char* app_path(const char* name)
{
    static char buf[256];
    if (bundle_command(name, buf, sizeof(buf)) != 0)
        buf[0] = '\0';
    return buf;
}
