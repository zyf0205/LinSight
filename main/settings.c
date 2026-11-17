#include "settings.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define TAG "settings"
#define NVS_NS "ls"
#define NVS_KEY_BOOT "boot"
#define NVS_KEY_BITRATE "br"
#define NVS_KEY_MODE "mode"

static uint32_t s_boot_count;
static can_bitrate_t s_bitrate = CAN_BITRATE_500K;
#if CONFIG_LINSIGHT_CAN_MODE_NORMAL
static can_mode_t s_mode = CAN_MODE_NORMAL;
#else
static can_mode_t s_mode = CAN_MODE_LISTEN_ONLY;
#endif
static bool s_has_bitrate;
static bool s_has_mode;

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

    uint8_t raw = 0;
    if (nvs_get_u8(h, NVS_KEY_BITRATE, &raw) == ESP_OK && raw <= CAN_BITRATE_1M) {
        s_bitrate = (can_bitrate_t)raw;
        s_has_bitrate = true;
    }
    if (nvs_get_u8(h, NVS_KEY_MODE, &raw) == ESP_OK && raw <= CAN_MODE_SELFTEST) {
        s_mode = (can_mode_t)raw;
        s_has_mode = true;
    }

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
    ESP_LOGI(TAG, "CAN config %s %s", can_bitrate_name(s_bitrate),
             can_mode_name(s_mode));
    return ESP_OK;
}

uint32_t linsight_boot_count(void)
{
    return s_boot_count;
}

void settings_load_can(can_bitrate_t *bitrate, can_mode_t *mode)
{
    if (bitrate && s_has_bitrate) {
        *bitrate = s_bitrate;
    }
    if (mode && s_has_mode) {
        *mode = s_mode;
    }
}

esp_err_t settings_save_can(can_bitrate_t bitrate, can_mode_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_BITRATE, (uint8_t)bitrate);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_bitrate = bitrate;
        s_mode = mode;
        s_has_bitrate = true;
        s_has_mode = true;
    }
    return err;
}
