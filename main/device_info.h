#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINSIGHT_FW_VERSION "2.1.0"

/* Stable identity for multi-device hosts and log metadata. */
const char *linsight_device_id(void);

/* Human-readable reset reason ("POWERON", "TASK_WDT", ...). */
const char *linsight_reset_reason(void);

/* LINSIGHT_CAP_* bits advertised in DEVICE_INFO. */
uint32_t linsight_caps(void);

#ifdef __cplusplus
}
#endif
