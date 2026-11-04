#pragma once
/* Host stand-in for ESP-IDF's esp_heap_caps.h (UI simulator only). */
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, unsigned caps) { (void)caps; return calloc(c, n); }
