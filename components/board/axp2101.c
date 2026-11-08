#include "axp2101.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "axp2101";
static i2c_master_dev_handle_t s_dev;

#define REG_STATUS1      0x00
#define REG_STATUS2      0x01
#define REG_CHIP_ID      0x03
#define REG_FUEL_GAUGE   0x18
#define REG_ADC_EN       0x30
#define REG_DCDC_ONOFF   0x80
#define REG_DC1_VOLT     0x82
#define REG_LDO_ONOFF0   0x90
#define REG_ALDO1_VOLT   0x92
#define REG_ALDO2_VOLT   0x93
#define REG_BAT_PERCENT  0xA4

#define AXP2101_CHIP_ID  0x4A

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t update(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(rd(reg, &v), TAG, "read 0x%02x", reg);
    uint8_t nv = (v & ~mask) | (val & mask);
    return nv == v ? ESP_OK : wr(reg, nv);
}

/* ALDO/BLDO: 500 mV + 100 mV * code (code 0..30) */
static esp_err_t set_ldo_mv(uint8_t reg, int mv)
{
    return update(reg, 0x1F, (uint8_t)((mv - 500) / 100));
}

esp_err_t axp2101_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add dev");

    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(rd(REG_CHIP_ID, &id), TAG, "no AXP2101 on I2C");
    if (id != AXP2101_CHIP_ID) {
        ESP_LOGW(TAG, "unexpected chip id 0x%02x (want 0x%02x)", id, AXP2101_CHIP_ID);
    }

    /* DCDC1 powers the SoC - must already be on; just make sure. */
    ESP_RETURN_ON_ERROR(update(REG_DCDC_ONOFF, 0x01, 0x01), TAG, "dcdc1");

    /* ALDO1/ALDO2: board 3.3 V peripheral rails. */
    ESP_RETURN_ON_ERROR(set_ldo_mv(REG_ALDO1_VOLT, 3300), TAG, "aldo1 v");
    ESP_RETURN_ON_ERROR(set_ldo_mv(REG_ALDO2_VOLT, 3300), TAG, "aldo2 v");
    ESP_RETURN_ON_ERROR(update(REG_LDO_ONOFF0, 0x03, 0x03), TAG, "aldo1/2 on");

    /* Battery / VBUS / VSYS voltage ADC channels + fuel gauge (for the battery readout). */
    update(REG_ADC_EN, 0x0D, 0x0D);
    update(REG_FUEL_GAUGE, 0x08, 0x08);

    uint8_t dc = 0, ldo = 0, dc1 = 0;
    rd(REG_DCDC_ONOFF, &dc);
    rd(REG_LDO_ONOFF0, &ldo);
    rd(REG_DC1_VOLT, &dc1);
    ESP_LOGI(TAG, "id=0x%02x DCDC en=0x%02x (DC1 %d mV) LDO en=0x%02x",
             id, dc, 1500 + (dc1 & 0x1F) * 100, ldo);
    return ESP_OK;
}

bool axp2101_vbus_present(void)
{
    uint8_t s = 0;
    return s_dev && rd(REG_STATUS1, &s) == ESP_OK && (s & 0x20);
}

int axp2101_battery_percent(void)
{
    uint8_t s = 0, p = 0;
    if (!s_dev || rd(REG_STATUS1, &s) != ESP_OK || !(s & 0x08)) {   /* bit3: battery present */
        return -1;
    }
    if (rd(REG_BAT_PERCENT, &p) != ESP_OK || p > 100) {
        return -1;
    }
    return p;
}
