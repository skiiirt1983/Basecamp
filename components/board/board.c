/*
 * Board bring-up for Waveshare ESP32-S3-Touch-AMOLED-1.75.
 * Pin map, CO5300 init table, column gap, rounder and touch orientation follow
 * the Waveshare BSP (waveshare/esp32_s3_touch_amoled_1_75 v3.0.1), without its
 * audio / SD / USB dependencies.
 */
#include "board.h"
#include "axp2101.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

#define PIN_I2C_SCL     14
#define PIN_I2C_SDA     15
#define PIN_LCD_CS      12
#define PIN_LCD_PCLK    38
#define PIN_LCD_D0      4
#define PIN_LCD_D1      5
#define PIN_LCD_D2      6
#define PIN_LCD_D3      7
#define PIN_LCD_RST     39
#define PIN_TP_RST      40
#define PIN_TP_INT      11
#define LCD_HOST        SPI2_HOST
#define LCD_X_GAP       6
#define DRAW_BUF_LINES  60

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

/* Brightness written by the init table. The panel's RAM holds random pixels until the first frame is
 * sent; at full brightness they show as a white flash. board_set_dark_start() makes it come up dark. */
static uint8_t s_init_brightness = 0xFF;

void board_set_dark_start(bool dark)
{
    s_init_brightness = dark ? 0x00 : 0xFF;
}

static const co5300_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, &s_init_brightness, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

/* CO5300 needs even start / odd end coordinates. */
static void rounder_cb(lv_area_t *a)
{
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

esp_err_t board_init(lv_display_t **out_disp)
{
    /* I2C (shared: AXP2101, touch, ...) */
    i2c_master_bus_handle_t i2c;
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c), TAG, "i2c");

    esp_err_t err = axp2101_init(i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 init failed (%s) - continuing on default rails", esp_err_to_name(err));
    }

    /* Display: CO5300 over QSPI */
    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1,
                                                             PIN_LCD_D2, PIN_LCD_D3,
                                                             BOARD_LCD_H_RES * DRAW_BUF_LINES * 2);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    co5300_vendor_config_t vendor = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), TAG, "panel");
    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* Touch: CST9217 */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c, &tp_io_cfg, &tp_io), TAG, "tp io");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t tp = NULL;
    err = esp_lcd_touch_new_i2c_cst9217(tp_io, &tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
    }

    /* LVGL */
    const lvgl_port_cfg_t lv_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lv_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .rounder_cb = rounder_cb,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = 1, .swap_bytes = 1},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add disp");

    if (tp) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = tp};
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGE(TAG, "lvgl touch registration failed");
        }
    }
    if (out_disp) {
        *out_disp = disp;
    }
    return ESP_OK;
}

void board_set_brightness(uint8_t percent)
{
    if (s_panel) {
        esp_lcd_panel_co5300_set_brightness(s_panel, percent > 100 ? 100 : percent);
    }
}

void board_display_off(void)
{
    if (s_panel) {
        board_set_brightness(0);
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
}
