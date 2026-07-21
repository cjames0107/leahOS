#include <leah/string.hpp>

extern "C" {

void* memset(void* dest, int value, usize count)
{
    auto* p = static_cast<u8*>(dest);
    const auto byte = static_cast<u8>(value);
    for (usize i = 0; i < count; ++i)
        p[i] = byte;
    return dest;
}

void* memcpy(void* dest, const void* src, usize count)
{
    auto* d = static_cast<u8*>(dest);
    const auto* s = static_cast<const u8*>(src);
    for (usize i = 0; i < count; ++i)
        d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, usize count)
{
    auto* d = static_cast<u8*>(dest);
    const auto* s = static_cast<const u8*>(src);

    if (d == s || count == 0)
        return dest;

    // Copy backwards when the ranges overlap with dest above src, otherwise
    // we would clobber source bytes before reading them.
    if (d < s) {
        for (usize i = 0; i < count; ++i)
            d[i] = s[i];
    } else {
        for (usize i = count; i > 0; --i)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void* a, const void* b, usize count)
{
    const auto* x = static_cast<const u8*>(a);
    const auto* y = static_cast<const u8*>(b);
    for (usize i = 0; i < count; ++i) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

usize strlen(const char* str)
{
    usize n = 0;
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
    return static_cast<int>(static_cast<u8>(*a)) - static_cast<int>(static_cast<u8>(*b));
}

int strncmp(const char* a, const char* b, usize count)
{
    for (usize i = 0; i < count; ++i) {
        if (a[i] != b[i] || a[i] == '\0')
            return static_cast<int>(static_cast<u8>(a[i])) - static_cast<int>(static_cast<u8>(b[i]));
    }
    return 0;
}

} // extern "C"
