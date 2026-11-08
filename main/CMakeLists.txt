idf_component_register(SRCS "main.c" "slots.c" "ui.c" "netmgr.c" "installer.c"
                       INCLUDE_DIRS "."
                       REQUIRES board app_update bootloader_support esp_partition nvs_flash
                                esp_wifi esp_netif esp_event esp_http_client esp_http_server
                                json mbedtls lwip)
