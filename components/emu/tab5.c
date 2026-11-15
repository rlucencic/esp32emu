/*
 * M5Stack Tab5 board glue, after esp32_basic (radio_c6.c, m5stack_tab5_board.c):
 *
 *   - battery charging: the IP2326 charges only with CHG_EN (IO expander
 *     0x44 pin 7) high, which nothing else sets: without it the pack never
 *     charges and the board dies when USB is unplugged
 *   - the battery's state (battery.h) from the INA226 (0x41), as M5Unified
 *     reads it: the pack's voltage, its current through a 5 mOhm shunt (wired
 *     so that charging reads negative), charging or not by that current; the
 *     IP2326's status line (0x44 pin 6) reads high whether it charges or not
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
#include "battery.h"
#include "tab5.h"

#define CHG_EN          IO_EXPANDER_PIN_NUM_7   /* expander 0x44 */
#define CHG_STAT        IO_EXPANDER_PIN_NUM_6
#define CHG_QC          IO_EXPANDER_PIN_NUM_5   /* nCHG_QC_EN: low the 1 A step, high 500 mA */

/* INA226 registers */
#define INA_CONFIG          0x00
#define INA_SHUNT           0x01    /* signed, 2.5 uV a step */
#define INA_BUS             0x02    /* 1.25 mV a step */
#define INA_CONFIG_VALUE    0x4527  /* 16 samples averaged, 1.1 ms each for both, continuous (M5Unified's) */
#define BATTERY_MIN_MV      5000    /* below: no pack (2S, protection cuts off at ~5 V) */
#define BATTERY_IDLE_MA     10      /* M5Unified's threshold for idle */
/* PI4IOE5V6408 registers */
#define REG_IO_DIRECTION    0x03
#define REG_OUTPUT          0x05
#define REG_HIGH_IMPEDANCE  0x07
#define REG_PULL_ENABLE     0x0B
#define REG_PULL_SELECT     0x0D    /* 1 up, 0 down */
#define REG_INPUT           0x0F    /* the pins' levels */
#define C6_POWER_BIT        0x01
#define CHG_EN_BIT          0x80
#define CHG_STAT_BIT        0x40
#define CHG_QC_BIT          0x20    /* low: the 1 A step */

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
static i2c_master_dev_handle_t s_ina;
static i2c_master_dev_handle_t s_charger_io;   /* expander 0x44, to read the charger's lines back */
static int s_battery_logged = -1;          /* the state last logged */
static int64_t s_battery_logged_at;

/* The charger's lines as M5Unified sets them: CHG_EN driven high, the QC
 * line driven low (its 1 A step), the status line an input (the
 * driver's reset leaves every pin a floating output)
 */
static void charging_on(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if (io == NULL ||
        esp_io_expander_set_dir(io, CHG_EN, IO_EXPANDER_OUTPUT) != ESP_OK ||
        esp_io_expander_set_output_mode(io, CHG_EN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) != ESP_OK ||
        esp_io_expander_set_level(io, CHG_EN, 1) != ESP_OK) {
        ESP_LOGW(TAG, "battery charging not enabled");
    }
    if (io == NULL ||
        esp_io_expander_set_dir(io, CHG_QC, IO_EXPANDER_OUTPUT) != ESP_OK ||
        esp_io_expander_set_output_mode(io, CHG_QC, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) != ESP_OK ||
        esp_io_expander_set_level(io, CHG_QC, 0) != ESP_OK ||
        esp_io_expander_set_dir(io, CHG_STAT, IO_EXPANDER_INPUT) != ESP_OK) {
        ESP_LOGW(TAG, "charger lines not set");
    }
}

static esp_err_t set_bit(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t bit, bool on);

static bool charger_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_charger_io, &reg, 1, value, 1, 100) == ESP_OK;
}

/* The charger's lines from the expander's own registers, not the driver's
 * copy: CHG_EN's level, whether it's an output driven high
 */
static void charger_read(battery_t *b)
{
    uint8_t *r = b->charger_regs;
    b->charger = b->charger_status = b->charger_fast = -1;
    b->charger_driven = false;
    if (s_charger_io == NULL || !charger_reg(REG_IO_DIRECTION, &r[0]) || !charger_reg(REG_OUTPUT, &r[1]) ||
        !charger_reg(REG_HIGH_IMPEDANCE, &r[2]) || !charger_reg(REG_PULL_ENABLE, &r[3]) ||
        !charger_reg(REG_PULL_SELECT, &r[4]) || !charger_reg(REG_INPUT, &r[5])) {
        return;
    }
    /* (the input register reads 0 for a pin set as an output, driven or
     * not: an output's state is its direction, high-Z and output bits)
     */
    uint8_t dir = r[0], out = r[1], high_z = r[2], in = r[5];
    b->charger_driven = (dir & CHG_EN_BIT) && !(high_z & CHG_EN_BIT) && (out & CHG_EN_BIT);
    b->charger = b->charger_driven;
    b->charger_status = (dir & CHG_STAT_BIT) ? -1 : (in & CHG_STAT_BIT) != 0;
    b->charger_fast = (dir & CHG_QC_BIT) && !(high_z & CHG_QC_BIT) ? !(out & CHG_QC_BIT) : -1;
}

/* The INA226 converting continuously (it may have been left powered down) */
static void battery_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CHARGER_EXPANDER,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_charger_io) != ESP_OK) {
        s_charger_io = NULL;
    } else {
        set_bit(s_charger_io, REG_PULL_ENABLE, CHG_STAT_BIT, false);   /* no pull-down on the status line (M5Unified) */
    }
    battery_t b;
    charger_read(&b);
    ESP_LOGI(TAG, "charger: CHG_EN %s, CHG_STAT %d, %s step", b.charger_driven ? "driven high" : "NOT driven high",
             b.charger_status, b.charger_fast == 1 ? "1 A" : b.charger_fast == 0 ? "500 mA" : "unknown");

    cfg.device_address = BOARD_INA226_ADDR;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_ina) != ESP_OK) {
        ESP_LOGW(TAG, "battery: no INA226");
        s_ina = NULL;
        return;
    }
    const uint8_t data[3] = { INA_CONFIG, INA_CONFIG_VALUE >> 8, INA_CONFIG_VALUE & 0xFF };
    if (i2c_master_transmit(s_ina, data, sizeof(data), 100) != ESP_OK) {
        ESP_LOGW(TAG, "battery: INA226 not answering");
        i2c_master_bus_rm_device(s_ina);
        s_ina = NULL;
    }
}

static bool ina_read(uint8_t reg, int16_t *value)
{
    uint8_t buf[2];
    if (i2c_master_transmit_receive(s_ina, &reg, 1, buf, sizeof(buf), 100) != ESP_OK) {
        return false;
    }
    *value = (int16_t)(buf[0] << 8 | buf[1]);
    return true;
}

bool battery_read(battery_t *b)
{
    int16_t shunt, bus;
    if (s_ina == NULL || !ina_read(INA_SHUNT, &shunt) || !ina_read(INA_BUS, &bus)) {
        return false;
    }
    charger_read(b);
    b->mv = (uint16_t)bus * 5 / 4;
    b->ma = -shunt * 5 / 2 / BOARD_SHUNT_MOHM;
    /* 2S: each cell's share, M5Unified's line (3.3 V empty, 4.15 V full) */
    int percent = (b->mv / 2 - 3300) * 100 / 800;
    b->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (b->mv < BATTERY_MIN_MV) {
        b->state = BATTERY_NONE;
        b->percent = 0;
    } else if (b->ma > BATTERY_IDLE_MA) {
        b->state = BATTERY_CHARGING;
    } else if (b->ma < -BATTERY_IDLE_MA) {
        b->state = BATTERY_DISCHARGING;
    } else {
        b->state = BATTERY_IDLE;
    }
    /* each change of state (near the idle threshold: one in 10 s at most) */
    int64_t now = esp_timer_get_time();
    if ((int)b->state != s_battery_logged && (s_battery_logged < 0 || now - s_battery_logged_at > 10000000)) {
        s_battery_logged = b->state;
        s_battery_logged_at = now;
        ESP_LOGI(TAG, "battery %s: %d mV, %d mA, %d%%", battery_state_name(b->state), b->mv, b->ma, b->percent);
    }
    return true;
}

const char *battery_state_name(battery_state_t state)
{
    static const char *const names[] = { "none", "discharging", "idle", "charging" };
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
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
    battery_init();
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
