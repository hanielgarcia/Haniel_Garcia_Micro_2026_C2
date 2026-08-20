idf_component_register(SRCS "reaction_timer.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif mqtt nvs_flash freertos driver esp_timer json esp_event)
