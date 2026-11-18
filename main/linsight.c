#include "bsp.h"
#include "can_ring.h"
#include "device_info.h"
#include "esp_log.h"
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
    linsight_ui_handle_key(event == BSP_BUTTON_LONG);
}

void app_main(void)
{
    ESP_LOGI(TAG, "device id: %s", linsight_device_id());

    ESP_ERROR_CHECK(bsp_init());
    const esp_err_t settings_err = settings_init();
    if (settings_err != ESP_OK) {
        /* Keep capturing with Kconfig defaults even if NVS is unavailable. */
        ESP_LOGE(TAG, "settings init failed: %s", esp_err_to_name(settings_err));
    }
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
    const esp_err_t net_err = net_start();
    if (net_err != ESP_OK) {
        /* A black box must keep recording even if networking cannot start. */
        ESP_LOGE(TAG, "net start failed: %s", esp_err_to_name(net_err));
    }

    ESP_LOGI(TAG, "LinSight ready (key: short=switch page, long=mark/clear)");
}
