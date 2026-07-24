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
