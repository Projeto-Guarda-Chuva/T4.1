#pragma once
#ifndef STRLCPY_COMPAT_DEFINED
#define STRLCPY_COMPAT_DEFINED
#include <cstring>
static inline size_t strlcpy(char* dst, const char* src, size_t n) {
    if (!n) return strlen(src);
    size_t i = 0;
    for (; i < n - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
    return strlen(src);
}
#endif
