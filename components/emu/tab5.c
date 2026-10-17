/*
 * M5Stack Tab5 board glue, after esp32_basic (radio_c6.c, m5stack_tab5_board.c):
 *
 *   - battery charging: the IP2326 charges only with CHG_EN (IO expander
 *     0x44 pin 7) high, which nothing else sets: without it the pack never
 *     charges and the board dies when USB is unplugged
 *   - the ESP32-C6 (WiFi, Bluetooth): powered by the BSP's WiFi feature and
 *     PI4IOE5V6408 0x44 pin 0, ~2 s to boot its SDIO slave, then
 *     esp_hosted; its Bluetooth controller is off until enabled
 *   - the RX8130CE real-time clock (0x32, a supercap keeps it running),
 *     kept in UTC like esp32_basic (wmbus_hub keeps local time): the system
 *     clock comes from it at boot when it holds a time, and goes into it
 *     whenever the internet sets it. An unset one reads as plausible
 *     garbage: VLF (the oscillator stopped) and the date are both checked.
 *     M5Stack's own firmware wrote tm_mon (0-11) into the month register,
 *     which counts 1-12: a clock it set reads a month early until then
 */
#include <string.h>
#include <sys/time.h>
#include <time.h>
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

/* RX8130CE registers */
#define RTC_ADDR            0x32
#define RTC_SEC             0x10    /* sec, min, hour, weekday (one bit each), day, month, year */
#define RTC_FLAG            0x1D
#define RTC_CTRL0           0x1E
#define RTC_CTRL1           0x1F
#define RTC_FLAG_VLF        0x02    /* the oscillator stopped: the time is lost */
#define RTC_CTRL0_STOP      0x40
#define RTC_CTRL1_BACKUP    0x30    /* INIEN, CHGEN: the backup supply, as M5Stack's firmware sets */

static const char *TAG = "tab5";
static int64_t s_c6_powered_at;
static i2c_master_dev_handle_t s_rtc;
static bool s_clock_loaded;

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

static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_rtc, &reg, 1, buf, n, 100);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t data[8] = { reg };
    memcpy(data + 1, buf, n);
    return i2c_master_transmit(s_rtc, data, n + 1, 100);
}

static int from_bcd(uint8_t v)
{
    return (v & 0x0F) > 9 || (v >> 4) > 9 ? -1 : (v >> 4) * 10 + (v & 0x0F);
}

static uint8_t to_bcd(int v)
{
    return (v / 10) << 4 | v % 10;
}

/* Days since 1970-01-01 */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void clock_load(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    uint8_t r[RTC_CTRL1 - RTC_SEC + 1];
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_rtc) != ESP_OK ||
        rtc_read(RTC_SEC, r, sizeof(r)) != ESP_OK) {
        ESP_LOGW(TAG, "clock: no RX8130");
        if (s_rtc != NULL) {
            i2c_master_bus_rm_device(s_rtc);
            s_rtc = NULL;
        }
        return;
    }
    uint8_t ctrl1 = r[RTC_CTRL1 - RTC_SEC];
    if ((ctrl1 & RTC_CTRL1_BACKUP) != RTC_CTRL1_BACKUP) {
        ctrl1 |= RTC_CTRL1_BACKUP;
        rtc_write(RTC_CTRL1, &ctrl1, 1);
    }
    int sec = from_bcd(r[0] & 0x7F), min = from_bcd(r[1] & 0x7F), hour = from_bcd(r[2] & 0x3F);
    int day = from_bcd(r[4] & 0x3F), month = from_bcd(r[5] & 0x1F), year = 2000 + from_bcd(r[6]);
    bool lost = r[RTC_FLAG - RTC_SEC] & RTC_FLAG_VLF;
    if (lost || sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23 ||
        day < 1 || day > 31 || month < 1 || month > 12 || year < 2024) {
        ESP_LOGI(TAG, "clock: not set (%02x %02x %02x %02x %02x %02x, %s)", r[6], r[5], r[4],
                 r[2], r[1], r[0], lost ? "stopped" : "no date");
        return;
    }
    /* Somewhere within that second; a software restart keeps the system
     * clock, which is closer then
     */
    struct timeval tv = {
        .tv_sec = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec,
        .tv_usec = 500000,
    };
    if (time(NULL) < tv.tv_sec - 60) {
        settimeofday(&tv, NULL);
    }
    s_clock_loaded = true;
    ESP_LOGI(TAG, "clock: %04d-%02d-%02d %02d:%02d:%02d UTC", year, month, day, hour, min, sec);
}

bool tab5_clock_loaded(void)
{
    return s_clock_loaded;
}

void tab5_clock_save(void)
{
    if (s_rtc == NULL) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec + (tv.tv_usec >= 500000);
    struct tm tm;
    gmtime_r(&t, &tm);
    const uint8_t date[7] = {
        to_bcd(tm.tm_sec), to_bcd(tm.tm_min), to_bcd(tm.tm_hour), 1 << tm.tm_wday,
        to_bcd(tm.tm_mday), to_bcd(tm.tm_mon + 1), to_bcd(tm.tm_year % 100),
    };
    /* Stopped while it's written; the flag of a lost time cleared after */
    uint8_t ctrl0, flags;
    esp_err_t err = rtc_read(RTC_CTRL0, &ctrl0, 1);
    if (err == ESP_OK) {
        ctrl0 |= RTC_CTRL0_STOP;
        err = rtc_write(RTC_CTRL0, &ctrl0, 1);
    }
    if (err == ESP_OK) {
        err = rtc_write(RTC_SEC, date, sizeof(date));
    }
    ctrl0 &= ~RTC_CTRL0_STOP;
    if (err == ESP_OK) {
        err = rtc_write(RTC_CTRL0, &ctrl0, 1);
    }
    if (err == ESP_OK) {
        err = rtc_read(RTC_FLAG, &flags, 1);
    }
    if (err == ESP_OK && (flags & RTC_FLAG_VLF)) {
        flags &= ~RTC_FLAG_VLF;
        err = rtc_write(RTC_FLAG, &flags, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clock: not saved (%s)", esp_err_to_name(err));
    }
}

void tab5_init(void)
{
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed");
        return;
    }
    clock_load();
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
