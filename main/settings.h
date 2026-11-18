#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "twai.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINSIGHT_FW_VERSION "2.1.0"

/* Stable 12-hex-char device identity derived from the Wi-Fi STA MAC. */
const char *linsight_device_id(void);

/* One-time NVS bring-up. Must run before net_start()/Wi-Fi. */
esp_err_t settings_init(void);

/* Monotonic boot counter stored in NVS; increments once per firmware boot. */
uint32_t linsight_boot_count(void);

/* Apply persisted CAN bitrate/mode, leaving the caller's values in place when
 * no valid value has been stored yet. Call after settings_init(). */
void settings_load_can(can_bitrate_t *bitrate, can_mode_t *mode);

/* Persist the active CAN bitrate/mode. Called after a successful restart. */
esp_err_t settings_save_can(can_bitrate_t bitrate, can_mode_t mode);

#ifdef __cplusplus
}
#endif
