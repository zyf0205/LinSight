/*
 * M0/bench probe: TWAI 500 kbps, print received CAN frames on UART0 and
 * transmit a test frame once per second (ID 0x7FF) so a USB-CAN receiver can
 * verify the board -> transceiver TX path.
 *
 * Bench mode is NORMAL so the ESP32 ACKs the USB-CAN transmitter. Do not
 * connect this build to a vehicle bus; switch PROBE_MODE to
 * TWAI_MODE_LISTEN_ONLY when capturing an unknown/live bus.
 */

#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "can_probe"

#define TX_GPIO GPIO_NUM_10
#define RX_GPIO GPIO_NUM_11
#define PROBE_MODE TWAI_MODE_NORMAL

/* An idle transceiver drives RXD high and pulls TXD high internally. With an
 * internal pull-down, a connected pin reads 1; a floating pin reads 0. */
static int pin_idle_level(gpio_num_t pin)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0,
        .pull_down_en = 1,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int level = gpio_get_level(pin);
    gpio_reset_pin(pin);
    return level;
}

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
    ESP_LOGI(TAG, "GPIO%d (RXD) idle with pull-down: %d (1 = transceiver powered)",
             RX_GPIO, pin_idle_level(RX_GPIO));
    ESP_LOGI(TAG, "GPIO%d (TXD) idle with pull-down: %d (1 = transceiver TXD connected)",
             TX_GPIO, pin_idle_level(TX_GPIO));

    const twai_general_config_t general =
        TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO, RX_GPIO, PROBE_MODE);
    const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&general, &timing, &filter));
    ESP_ERROR_CHECK(twai_start());

    ESP_LOGI(TAG, "TWAI 500 kbps, TX=GPIO%d RX=GPIO%d, mode=%s", TX_GPIO, RX_GPIO,
             PROBE_MODE == TWAI_MODE_NORMAL ? "NORMAL (bench, ACKs)" : "LISTEN_ONLY");

    uint32_t frames = 0;
    uint32_t tx_seq = 0;
    int64_t last_stats = esp_timer_get_time();
    int64_t last_tx = last_stats;

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
        if (now - last_tx >= 1000000) {
            twai_message_t tx = {0};
            tx.identifier = 0x7FF;
            tx.data_length_code = 8;
            for (int i = 0; i < 8; i++) {
                tx.data[i] = (uint8_t)(tx_seq + (uint32_t)i);
            }
            const esp_err_t tx_err = twai_transmit(&tx, pdMS_TO_TICKS(50));
            ESP_LOGI(TAG, "TX 0x7FF seq=%" PRIu32 ": %s", tx_seq,
                     esp_err_to_name(tx_err));
            tx_seq++;
            last_tx = now;
        }
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
