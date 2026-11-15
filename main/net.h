#pragma once

#include "esp_err.h"

/* Starts the TCP client task, which brings up Wi-Fi first (HELLO, CAN_BATCH,
 * EVENT, PING/PONG). Safe to call once from app_main after the capture and UI
 * exist. */
esp_err_t net_start(void);
