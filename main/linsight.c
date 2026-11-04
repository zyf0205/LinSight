#include "bsp.h"
#include "can_ring.h"
#include "driver/twai.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_lvgl_port.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "twai.h"
#include "ui.h"

#include "sdkconfig.h"

static const char *TAG = "linsight";

static int s_saved_brightness = 80;

static void user_button_cb(bsp_button_event_t event)
{
    const int level = bsp_backlight_get();

    if (event == BSP_BUTTON_SHORT) {
        if (level == 0) {
            /* Wake the screen only. */
            bsp_backlight_set(s_saved_brightness > 0 ? s_saved_brightness : 80);
            return;
        }
        can_stats_t st;
        twai_stats_snapshot(&st);
        twai_capture_set_paused(!st.paused);
        return;
    }

    /* Long press: settings page (or toggle the backlight from there). */
    if (level > 0) {
        s_saved_brightness = level;
        bsp_backlight_set(0);
    } else {
        bsp_backlight_set(s_saved_brightness > 0 ? s_saved_brightness : 80);
        linsight_ui_toggle_settings();
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip model=%d cores=%d revision=%d", chip.model, chip.cores, chip.revision);
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());

    ESP_ERROR_CHECK(bsp_init());

    /* 1 MB PSRAM ring at 20 B/record: ~52k frames of offline buffering. */
    ESP_ERROR_CHECK(can_ring_init(1048576u / LINSIGHT_REC_SIZE));
    ESP_ERROR_CHECK(twai_capture_init());

#if CONFIG_LINSIGHT_BENCH_SELFTEST
    can_mode_t mode = CAN_MODE_SELFTEST;
#elif CONFIG_LINSIGHT_CAN_MODE_NORMAL
    can_mode_t mode = CAN_MODE_NORMAL;
#else
    can_mode_t mode = CAN_MODE_LISTEN_ONLY;
#endif

    lvgl_port_lock(0);
    linsight_ui_init();
    lvgl_port_unlock();

    /* Let the first splash frame replace the panel's old GRAM content before
       the backlight comes on; otherwise the previous UI flashes for a moment. */
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(bsp_backlight_set(80));
    ESP_ERROR_CHECK(bsp_user_button_init(user_button_cb));

    ESP_ERROR_CHECK(twai_capture_start(CAN_BITRATE_500K, mode));
    ESP_ERROR_CHECK(net_start());

#if CONFIG_LINSIGHT_BENCH_SELFTEST
    /* Bench load generator: flood self-received frames at bus rate to verify
     * the capture core (task/ring/UI) without an external node. */
    twai_message_t burst = {
        .identifier = 0x123,
        .data_length_code = 8,
        .data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
        .self = 1,
    };
    uint32_t sent = 0;
    while (sent < 30000) {
        if (twai_transmit(&burst, pdMS_TO_TICKS(100)) == ESP_OK) {
            sent++;
        } else {
            break;
        }
    }
    ESP_LOGI(TAG, "selftest burst queued: %u", (unsigned)sent);
#endif

    ESP_LOGI(TAG, "LinSight CAN ready (key: short=pause, long=settings)");
}
