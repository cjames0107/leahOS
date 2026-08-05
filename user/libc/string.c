#include <string.h>

void* memcpy(void* dest, const void* src, size_t count)
{
    unsigned char* d = dest;
    const unsigned char* s = src;
    for (size_t i = 0; i < count; ++i)
        d[i] = s[i];
    return dest;
}

void* memset(void* dest, int value, size_t count)
{
    unsigned char* d = dest;
    for (size_t i = 0; i < count; ++i)
        d[i] = (unsigned char)value;
    return dest;
}

void* memmove(void* dest, const void* src, size_t count)
{
    unsigned char* d = dest;
    const unsigned char* s = src;
    if (d == s || count == 0)
        return dest;
    if (d < s) {
        for (size_t i = 0; i < count; ++i)
            d[i] = s[i];
    } else {
        for (size_t i = count; i > 0; --i)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t count)
{
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (size_t i = 0; i < count; ++i) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char* str)
{
    size_t n = 0;
    while (str[n] != '\0')
        ++n;
    return n;
}

int strcmp(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char* strcpy(char* dest, const char* src)
{
    char* out = dest;
    while ((*out++ = *src++) != '\0')
        ;
    return dest;
}

/* --- the ones that were missing ---------------------------------------------
 *
 * Added when the environment needed them, which is the usual way a libc grows:
 * something wants a standard function, finds it absent, and either writes a
 * private copy or the function gets written properly once. A private copy in
 * four files is how a system ends up with four subtly different searches.
 */

int strncmp(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y)
            return x < y ? -1 : 1;
        if (x == '\0')
            return 0;
    }
    return 0;
}

char* strchr(const char* s, int c)
{
    const char want = (char)c;
    for (;; ++s) {
        if (*s == want)
            return (char*)s;
        if (*s == '\0')
            return 0;       /* the terminator counts, which is why this order */
    }
}

char* strrchr(const char* s, int c)
{
    const char want = (char)c;
    const char* found = 0;
    for (;; ++s) {
        if (*s == want)
            found = s;
        if (*s == '\0')
            return (char*)found;
    }
}

char* strstr(const char* haystack, const char* needle)
{
    if (needle[0] == '\0')
        return (char*)haystack;
    for (; *haystack != '\0'; ++haystack) {
        size_t i = 0;
        while (needle[i] != '\0' && haystack[i] == needle[i])
            ++i;
        if (needle[i] == '\0')
            return (char*)haystack;
    }
    return 0;
}

char* strncpy(char* dst, const char* src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i)
        dst[i] = src[i];
    /* Padded with zeros to the full length, which is what strncpy does and the
     * reason it does not always terminate: if src fills n exactly there is no
     * room left for a terminator. Callers wanting one must add it. */
    for (; i < n; ++i)
        dst[i] = '\0';
    return dst;
}
