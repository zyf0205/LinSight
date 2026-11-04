/*
 * M0 bench probe: TWAI 500 kbps, print received CAN frames on UART0.
 *
 * Bench mode is NORMAL so the ESP32 ACKs the USB-CAN transmitter. Do not
 * connect this build to a vehicle bus; switch PROBE_MODE to
 * TWAI_MODE_LISTEN_ONLY when capturing an unknown/live bus.
 */

#include <inttypes.h>
#include <stdio.h>

#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "can_probe"

#define TX_GPIO GPIO_NUM_10
#define RX_GPIO GPIO_NUM_11
#define PROBE_MODE TWAI_MODE_NORMAL

static const char *state_name(twai_state_t state)
{
    switch (state) {
    case TWAI_STATE_STOPPED:
        return "STOPPED";
    case TWAI_STATE_RUNNING:
        return "RUNNING";
    case TWAI_STATE_BUS_OFF:
        return "BUS_OFF";
    case TWAI_STATE_RECOVERING:
        return "RECOVERING";
    default:
        return "?";
    }
}

void app_main(void)
{
    const twai_general_config_t general =
        TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO, RX_GPIO, PROBE_MODE);
    const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&general, &timing, &filter));
    ESP_ERROR_CHECK(twai_start());

    ESP_LOGI(TAG, "TWAI 500 kbps, TX=GPIO%d RX=GPIO%d, mode=%s", TX_GPIO, RX_GPIO,
             PROBE_MODE == TWAI_MODE_NORMAL ? "NORMAL (bench, ACKs)" : "LISTEN_ONLY");

    uint32_t frames = 0;
    int64_t last_stats = esp_timer_get_time();

    for (;;) {
        twai_message_t msg;
        const esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(100));

        if (err == ESP_OK) {
            frames++;
            const int64_t now = esp_timer_get_time();
            printf("FRAME,%" PRId64 ",%c,%03" PRIX32 ",%u",
                   now, msg.extd ? 'X' : 'S', msg.identifier, msg.data_length_code);
            for (int i = 0; i < msg.data_length_code; i++) {
                printf(",%02X", msg.data[i]);
            }
            printf("\n");
            fflush(stdout);
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "twai_receive: %s", esp_err_to_name(err));
        }

        uint32_t alerts = 0;
        if (twai_read_alerts(&alerts, 0) == ESP_OK && alerts != 0) {
            if (alerts & TWAI_ALERT_BUS_OFF) {
                ESP_LOGW(TAG, "BUS-OFF: initiating recovery");
                twai_initiate_recovery();
            }
            if (alerts & TWAI_ALERT_BUS_RECOVERED) {
                ESP_LOGW(TAG, "bus recovered: restarting");
                twai_start();
            }
            if (alerts & (TWAI_ALERT_ERR_PASS | TWAI_ALERT_ABOVE_ERR_WARN |
                          TWAI_ALERT_BUS_ERROR)) {
                ESP_LOGW(TAG, "error alert 0x%08" PRIX32, alerts);
            }
            if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
                ESP_LOGW(TAG, "RX queue full alert 0x%08" PRIX32, alerts);
            }
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_stats >= 1000000) {
            twai_status_info_t status;
            if (twai_get_status_info(&status) == ESP_OK) {
                ESP_LOGI(TAG,
                         "frames=%" PRIu32 " state=%s rxq=%" PRIu32
                         " rx_err=%" PRIu32 " tx_err=%" PRIu32
                         " missed=%" PRIu32 " bus_err=%" PRIu32,
                         frames, state_name(status.state), status.msgs_to_rx,
                         status.rx_error_counter, status.tx_error_counter,
                         status.rx_missed_count, status.bus_error_count);
            }
            last_stats = now;
        }
    }
}
