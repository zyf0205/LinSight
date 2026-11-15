#include "bsp.h"
#include "can_ring.h"
#include "device_info.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
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
    ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "device id: %s", linsight_device_id());

    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(settings_init());
    sd_store_init();

    /* 4 MiB PSRAM ring (~209k records). */
    ESP_ERROR_CHECK(
        can_ring_init((4u * 1024u * 1024u) / sizeof(linsight_can_record_t)));
    ESP_ERROR_CHECK(twai_capture_init());

#if CONFIG_LINSIGHT_CAN_MODE_NORMAL
    can_mode_t mode = CAN_MODE_NORMAL;
#else
    can_mode_t mode = CAN_MODE_LISTEN_ONLY;
#endif
    can_bitrate_t bitrate = CAN_BITRATE_500K;
    settings_load_can(&bitrate, &mode);

    lvgl_port_lock(0);
    linsight_ui_init();
    lvgl_port_unlock();

    /* Let the first UI frame replace the panel's old GRAM content before the
       backlight comes on; otherwise the previous image flashes for a moment. */
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(bsp_backlight_set(80));
    ESP_ERROR_CHECK(bsp_user_button_init(user_button_cb));

    ESP_ERROR_CHECK(twai_capture_start(bitrate, mode));
    ESP_LOGI(TAG, "heap before net: internal=%u largest=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    const esp_err_t net_err = net_start();
    if (net_err != ESP_OK) {
        /* A black box must keep recording even if networking cannot start. */
        ESP_LOGE(TAG, "net start failed: %s", esp_err_to_name(net_err));
    }

    ESP_LOGI(TAG, "LinSight CAN ready (key: short=cycle views, long=view action)");
}
