/*
 * M5Stack Tab5 Keyboard (STM32, 14x5 keys) on Ext.Port1 I2C, after
 * esp32_basic's tab5_keyboard.c. In HID mode each event (register 0x30) is
 * [modifiers, usage]: a key down, or [modifiers, 0] when it's let go; an
 * empty queue reads 0xFF 0xFF. It reports one key at a time: a second key
 * down replaces the first.
 */
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emu/board.h"
#include "tab5.h"

#define KBD_I2C_HZ          100000
#define POLL_MS             10
#define MAX_EVENTS          8           /* per poll */

#define REG_INT_STAT        0x01        /* 0: clear pending interrupts */
#define REG_EVENT_NUM       0x02        /* 0: clear the queue */
#define REG_MODE_KEYBOARD   0x10        /* 0 normal, 1 HID, 2 characters */
#define REG_HID_EVENT       0x30
#define REG_FIRMWARE_VER    0xFE
#define MODE_HID            1

static const char *TAG = "tab5_kbd";
static i2c_master_dev_handle_t s_dev;
static void (*s_key)(uint8_t usage, bool pressed);

static esp_err_t kbd_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 50);
}

static esp_err_t kbd_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

/* Modifier bits b0-b7 are usages $E0-$E7 */
static void modifiers(uint8_t *now, uint8_t want, bool pressing)
{
    for (int b = 0; b < 8; b++) {
        uint8_t bit = 1 << b;
        if ((*now ^ want) & bit && ((want & bit) != 0) == pressing) {
            s_key(0xE0 + b, pressing);
            *now ^= bit;
        }
    }
}

static void keyboard_task(void *arg)
{
    uint8_t mods = 0, held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        for (int i = 0; i < MAX_EVENTS; i++) {
            uint8_t ev[2];
            if (kbd_read(REG_HID_EVENT, ev, sizeof(ev)) != ESP_OK || ev[1] == 0xFF) {
                break;
            }
            uint8_t want = ev[0], usage = ev[1];
            if (usage != 0) {
                /* Down: new modifiers first, then the key */
                if (held && held != usage) {
                    s_key(held, false);
                }
                modifiers(&mods, want, true);
                modifiers(&mods, want, false);
                s_key(usage, true);
                held = usage;
            } else {
                /* Up: the key first, then the modifiers */
                if (held) {
                    s_key(held, false);
                    held = 0;
                }
                modifiers(&mods, want, false);
                modifiers(&mods, want, true);
            }
        }
    }
}

bool tab5_keyboard_start(void (*key)(uint8_t usage, bool pressed))
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_KBD_I2C_PORT,
        .sda_io_num = BOARD_KBD_GPIO_SDA,
        .scl_io_num = BOARD_KBD_GPIO_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "Ext.Port1 I2C bus failed");
        return false;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_KBD_ADDR,
        .scl_speed_hz = KBD_I2C_HZ,
    };
    uint8_t version = 0;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK ||
        kbd_read(REG_FIRMWARE_VER, &version, 1) != ESP_OK) {
        ESP_LOGI(TAG, "no Tab5 Keyboard");
        if (s_dev) {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
        i2c_del_master_bus(bus);
        return false;
    }
    kbd_write(REG_MODE_KEYBOARD, MODE_HID);
    kbd_write(REG_INT_STAT, 0);
    kbd_write(REG_EVENT_NUM, 0);
    s_key = key;
    xTaskCreatePinnedToCore(keyboard_task, "tab5_kbd", 3072, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "Tab5 Keyboard (firmware %u)", version);
    return true;
}
