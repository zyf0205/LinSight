#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define LINSIGHT_FW_VERSION "2.1.0"

/* Stable identity for multi-device hosts and log metadata. */
const char *linsight_device_id(void);

/* Human-readable reset reason ("POWERON", "TASK_WDT", ...). */
const char *linsight_reset_reason(void);

#ifdef __cplusplus
}
#endif
