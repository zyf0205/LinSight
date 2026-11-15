#pragma once

#include "esp_err.h"

/* Starts Wi-Fi and the TCP client task (HELLO, CAN_BATCH, EVENT, PING/PONG).
 * Safe to call once from app_main after the capture and UI exist. */
esp_err_t net_start(void);
