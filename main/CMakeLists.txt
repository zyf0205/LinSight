idf_component_register(SRCS "linsight.c" "bsp.c" "twai.c" "can_ring.c" "ui.c" "net.c"
                            "settings.c" "device_info.c" "sd_store.c"
                            "../protocol/frame.c"
                    INCLUDE_DIRS "." "../protocol"
                    REQUIRES esp_wifi esp_event esp_netif nvs_flash json esp_timer
                             esp_driver_gpio esp_driver_twai esp_driver_sdmmc
                             esp_psram esp_lcd driver fatfs vfs esp_hw_support
                             esp_common heap)
