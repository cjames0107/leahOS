#include <clipboard.h>
#include <shm.h>
#include <string.h>

/* Header and text in one segment: the length has to travel with the bytes, and
 * a second segment for four numbers would be silly. */
struct clip {
    volatile unsigned generation;
    volatile unsigned length;
    char text[CLIP_MAX];
};

static struct clip* g_clip;

/* Created by whoever copies first rather than by a daemon - there is no session
 * process to own it, and the first writer is as good a creator as any. */
static struct clip* clipboard(void)
{
    if (g_clip != 0)
        return g_clip;
    const int id = shm_open(CLIP_KEY, sizeof(struct clip), SHM_PUBLIC);
    if (id < 0)
        return 0;
    g_clip = (struct clip*)shm_map(id);
    return g_clip;
}

int clip_put(const char* text, unsigned length)
{
    struct clip* c = clipboard();
    if (c == 0)
        return -1;
    if (length > CLIP_MAX - 1)
        length = CLIP_MAX - 1;
    for (unsigned i = 0; i < length; ++i)
        c->text[i] = text[i];
    c->text[length] = '\0';
    /* Length before generation: a reader that sees the new generation must not
     * then read a stale length. */
    c->length = length;
    __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELEASE);
    return 0;
}

int clip_get(char* out, unsigned max)
{
    struct clip* c = clipboard();
    if (c == 0 || max == 0)
        return -1;
    unsigned n = c->length;
    if (n > max - 1)
        n = max - 1;
    for (unsigned i = 0; i < n; ++i)
        out[i] = c->text[i];
    out[n] = '\0';
    return (int)n;
}

unsigned clip_generation(void)
{
    struct clip* c = clipboard();
    return c != 0 ? __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) : 0;
}
