/*
 * M5Stack Tab5 board glue, after esp32_basic (radio_c6.c, m5stack_tab5_board.c):
 *
 *   - battery charging: the IP2326 charges only with CHG_EN (IO expander
 *     0x44 pin 7) high, which nothing else sets: without it the pack never
 *     charges and the board dies when USB is unplugged
 *   - the ESP32-C6 (WiFi, Bluetooth): powered by the BSP's WiFi feature and
 *     PI4IOE5V6408 0x44 pin 0, ~2 s to boot its SDIO slave, then
 *     esp_hosted; its Bluetooth controller is off until enabled
 */
#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emu/board.h"
#include "tab5.h"

#define CHG_EN          IO_EXPANDER_PIN_NUM_7   /* expander 0x44 */

/* PI4IOE5V6408 registers */
#define REG_IO_DIRECTION    0x03
#define REG_OUTPUT          0x05
#define REG_HIGH_IMPEDANCE  0x07
#define C6_POWER_BIT        0x01

static const char *TAG = "tab5";
static int64_t s_c6_powered_at;

static void charging_on(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if (io == NULL ||
        esp_io_expander_set_dir(io, CHG_EN, IO_EXPANDER_OUTPUT) != ESP_OK ||
        esp_io_expander_set_output_mode(io, CHG_EN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) != ESP_OK ||
        esp_io_expander_set_level(io, CHG_EN, 1) != ESP_OK) {
        ESP_LOGW(TAG, "battery charging not enabled");
    }
}

/* Read-modify-write one bit of an expander register */
static esp_err_t set_bit(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t bit, bool on)
{
    uint8_t v;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 100);
    if (err == ESP_OK) {
        uint8_t data[2] = { reg, on ? (v | bit) : (v & ~bit) };
        err = i2c_master_transmit(dev, data, sizeof(data), 100);
    }
    return err;
}

/* The C6's power pin, driven directly too (esp-hosted-mcu#127) */
static void c6_power_on(void)
{
    bsp_feature_enable(BSP_FEATURE_WIFI, true);
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_C6_EXPANDER_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "C6 power: no IO expander");
        return;
    }
    esp_err_t err = set_bit(dev, REG_IO_DIRECTION, C6_POWER_BIT, true);
    if (err == ESP_OK) {
        err = set_bit(dev, REG_HIGH_IMPEDANCE, C6_POWER_BIT, false);
    }
    if (err == ESP_OK) {
        err = set_bit(dev, REG_OUTPUT, C6_POWER_BIT, true);
    }
    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 power: %s", esp_err_to_name(err));
    }
}

void tab5_init(void)
{
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed");
        return;
    }
    charging_on();
    c6_power_on();
    s_c6_powered_at = esp_timer_get_time();
}

bool tab5_radio_start(void)
{
    /* The C6 boots while the screen comes up */
    int64_t ready = s_c6_powered_at + BOARD_C6_BOOT_MS * 1000LL;
    int64_t now = esp_timer_get_time();
    if (now < ready) {
        vTaskDelay(pdMS_TO_TICKS((ready - now) / 1000 + 1));
    }
    /* esp_hosted's constructor tried at boot, before the C6 had power; this
     * connects now
     */
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool tab5_bt_start(void)
{
    esp_err_t err = esp_hosted_bt_controller_init();
    if (err == ESP_OK) {
        err = esp_hosted_bt_controller_enable();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 Bluetooth controller: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
