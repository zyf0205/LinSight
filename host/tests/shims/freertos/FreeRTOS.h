#pragma once

/* Minimal host-side stand-in for FreeRTOS.h (ring test only). */

#include <stdint.h>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)

typedef void *SemaphoreHandle_t;
