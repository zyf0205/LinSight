#include "settings.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "settings"
#define NVS_NS "ls"
#define NVS_KEY_BOOT "boot"

static uint32_t s_boot_count;

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase");
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t boot = 0;
    const esp_err_t get = nvs_get_u32(h, NVS_KEY_BOOT, &boot);
    if (get != ESP_OK && get != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "boot counter read: %s", esp_err_to_name(get));
    }
    boot++;

    err = nvs_set_u32(h, NVS_KEY_BOOT, boot);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "boot counter write: %s", esp_err_to_name(err));
        return err;
    }
    s_boot_count = boot;
    ESP_LOGI(TAG, "boot count %u", (unsigned)s_boot_count);
    return ESP_OK;
}

uint32_t linsight_boot_count(void)
{
    return s_boot_count;
}
