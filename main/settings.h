#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time NVS bring-up. Must run before net_start()/Wi-Fi. */
esp_err_t settings_init(void);

/* Monotonic boot counter stored in NVS; increments once per power-on. */
uint32_t linsight_boot_count(void);

#ifdef __cplusplus
}
#endif
