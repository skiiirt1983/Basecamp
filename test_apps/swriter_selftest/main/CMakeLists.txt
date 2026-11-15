set(L "${CMAKE_CURRENT_LIST_DIR}/../../../main")
idf_component_register(SRCS "test_main.c" "${L}/swriter.c" "${L}/meta.c" "${L}/slots.c" "${L}/apps_logic.c"
                       INCLUDE_DIRS "${L}" "../../../common/include"
                       REQUIRES app_update bootloader_support esp_partition nvs_flash mbedtls spi_flash)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_BASECAMP_VERIFY_SLOTS=1)
