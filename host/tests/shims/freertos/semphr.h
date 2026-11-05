#pragma once

/* Minimal host-side stand-in for FreeRTOS's semphr.h (ring test only). */

#include <stdint.h>

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t sem);
