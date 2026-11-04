idf_component_register(SRCS "can_probe.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_twai esp_timer)
