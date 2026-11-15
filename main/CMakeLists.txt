idf_component_register(SRCS "main.c" "slots.c" "meta.c" "swriter.c" "ui.c" "netmgr.c" "webui.c"
                            "installer.c" "apps_logic.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "webui.html"
                       REQUIRES board app_update bootloader_support esp_partition nvs_flash
                                esp_wifi esp_netif esp_event esp_http_client esp_http_server
                                json mbedtls lwip)
