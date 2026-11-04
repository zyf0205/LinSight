#pragma once

#include "esp_err.h"

/* Starts Wi-Fi, the TCP client task, STATS/EVENT reporting and CONTROL
 * handling. Safe to call once from app_main after the capture and UI exist. */
esp_err_t net_start(void);
