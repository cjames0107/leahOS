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


int   strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
/* Pads to `n` with zeros and does not terminate when src fills it exactly,
 * which is what strncpy has always done and why it surprises people. */
char* strncpy(char* dst, const char* src, size_t n);

#endif /* _STRING_H */
