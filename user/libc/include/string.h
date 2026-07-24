#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void*  memcpy(void* dest, const void* src, size_t count);
void*  memset(void* dest, int value, size_t count);
void*  memmove(void* dest, const void* src, size_t count);
int    memcmp(const void* a, const void* b, size_t count);
size_t strlen(const char* str);
int    strcmp(const char* a, const char* b);
char*  strcpy(char* dest, const char* src);

#endif /* _STRING_H */
