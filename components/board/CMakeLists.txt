idf_component_register(SRCS "axp2101.c" "board.c"
                       INCLUDE_DIRS "include" "../../common/include"
                       REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio esp_lcd esp_hw_support
                       PRIV_REQUIRES esp_timer)
