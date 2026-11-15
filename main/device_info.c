#include "device_info.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_mac.h"

static char s_device_id[13];
static bool s_id_ready;

const char *linsight_device_id(void)
{
    if (!s_id_ready) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X", mac[0],
                 mac[1], mac[2], mac[3], mac[4], mac[5]);
        s_id_ready = true;
    }
    return s_device_id;
}
