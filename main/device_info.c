#include "device_info.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_mac.h"
#include "esp_system.h"

static char s_device_id[13];
static bool s_id_ready;

const char *linsight_device_id(void)
{
    if (!s_id_ready) {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X", mac[0],
                     mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            snprintf(s_device_id, sizeof(s_device_id), "000000000000");
        }
        s_id_ready = true;
    }
    return s_device_id;
}

const char *linsight_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "UNKNOWN";
    }
}
