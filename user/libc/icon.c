/* Loading the system's icons, and deciding which one a thing gets.
 *
 * The deciding is here rather than in the file browser because the browser is
 * not the only thing that shows files: the desktop does too, and two answers to
 * "what does a .PNG look like" is one answer too many.
 */

#include <icon.h>
#include <image.h>
#include <stdio.h>
#include <string.h>

#define ICON_DIR   "/share/icons"
#define CACHE_MAX  24

/* A miss is cached as well as a hit - `pixels` of 0 with the path filled in.
 * Without that, a folder full of files with no icon would go to the disk for
 * every one of them, on every repaint. */
static struct {
    char            path[192];
    const uint32_t* pixels;
    int             used;
} g_cache[CACHE_MAX];

static int g_count;

const uint32_t* icon_by_path(const char* path)
{
    int i;
    for (i = 0; i < g_count; ++i)
        if (g_cache[i].used && strcmp(g_cache[i].path, path) == 0)
            return g_cache[i].pixels;

    unsigned w = 0, h = 0;
    const uint32_t* px = img_read_png(path, &w, &h);
    /* Only the one size. A 64x64 icon would draw over its neighbours, and
     * scaling it here would mean every caller silently getting something other
     * than what is on disk. */
    if (px != 0 && (w != ICON_SIZE || h != ICON_SIZE))
        px = 0;

    if (g_count < CACHE_MAX) {
        i = g_count++;
        snprintf(g_cache[i].path, sizeof(g_cache[i].path), "%s", path);
        g_cache[i].pixels = px;
        g_cache[i].used = 1;
    }
    return px;
}

const uint32_t* icon_by_name(const char* name)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s.png", ICON_DIR, name);
    return icon_by_path(path);
}

/* Case-insensitive test for a trailing extension. The names on this filesystem
 * are upper case and the ones a person types are not, so neither side can be
 * assumed. */
static int ends_with(const char* s, const char* suffix)
{
    const int n = (int)strlen(s), m = (int)strlen(suffix);
    if (n < m)
        return 0;
    for (int i = 0; i < m; ++i) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b)
            return 0;
    }
    return 1;
}

const uint32_t* icon_for_entry(const char* dir_path, const char* name,
                               int is_dir, int is_app)
{
    if (is_app) {
        /* A bundle's icon is the bundle's own business, which is the point of
         * it being a directory with a picture in it. */
        if (dir_path != 0) {
            char path[192];
            const char* sep = (dir_path[0] != '\0' &&
                               dir_path[strlen(dir_path) - 1] == '/') ? "" : "/";
            snprintf(path, sizeof(path), "%s%s%s/Icon.png", dir_path, sep, name);
            const uint32_t* own = icon_by_path(path);
            if (own != 0)
                return own;
        }
        return icon_by_name("elements");
    }

    if (is_dir)
        return icon_by_name("folder-empty");

    if (ends_with(name, ".png") || ends_with(name, ".gif") ||
        ends_with(name, ".jpg") || ends_with(name, ".jpeg"))
        return icon_by_name("images");
    if (ends_with(name, ".elf"))
        return icon_by_name("binary");
    if (ends_with(name, ".txt") || ends_with(name, ".md") ||
        ends_with(name, ".c") || ends_with(name, ".h") ||
        ends_with(name, ".leahrc") || ends_with(name, ".alias"))
        return icon_by_name("edit");

    return icon_by_name("file");
}
