#include "bsp.h"
#include "can_ring.h"
#include "device_info.h"
#include "driver/twai.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "sd_store.h"
#include "settings.h"
#include "twai.h"
#include "ui.h"

#include "sdkconfig.h"

static const char *TAG = "linsight";

static void user_button_cb(bsp_button_event_t event)
{
    if (bsp_backlight_get() == 0) {
        /* Dark screen: any key press only wakes it (design §4). */
        bsp_backlight_set(80);
        return;
    }
    linsight_ui_handle_key(event == BSP_BUTTON_LONG);
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip model=%d cores=%d revision=%d", chip.model, chip.cores, chip.revision);
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());
    ESP_LOGI(TAG, "reset reason: %s", linsight_reset_reason());
    ESP_LOGI(TAG, "device id: %s", linsight_device_id());

    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(sd_store_init());

    /* Prefer a 4 MiB PSRAM ring (~209k records); fall back gracefully when the
     * network/LVGL allocations leave less headroom than expected. */
    static const uint32_t ring_bytes[] = {4u * 1024u * 1024u, 2u * 1024u * 1024u,
                                          1u * 1024u * 1024u};
    esp_err_t ring_err = ESP_ERR_NO_MEM;
    for (size_t i = 0; i < sizeof(ring_bytes) / sizeof(ring_bytes[0]); i++) {
        ring_err = can_ring_init(ring_bytes[i] / sizeof(linsight_can_record_t));
        if (ring_err == ESP_OK) {
            ESP_LOGI(TAG, "ring %u records (%u bytes)", (unsigned)(ring_bytes[i] /
                                                                   sizeof(linsight_can_record_t)),
                     (unsigned)ring_bytes[i]);
            break;
        }
    }
    ESP_ERROR_CHECK(ring_err);
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

    /* Let the first UI frame replace the panel's old GRAM content before the
       backlight comes on; otherwise the previous image flashes for a moment. */
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
