#pragma once

/* Minimal host-side stand-in for ESP-IDF's esp_heap_caps.h (ring test only). */

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)

void *heap_caps_malloc(size_t size, uint32_t caps);
