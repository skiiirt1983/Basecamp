idf_component_register(SRCS "main.c" "slots.c" "ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES board app_update bootloader_support esp_partition nvs_flash)
