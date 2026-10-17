/*
 * M5Stack Tab5 — ESP32-P4 (RISC-V, 2 cores at 360 MHz), 16 MB flash, 32 MB
 * PSRAM, an ESP32-C6 for WiFi and Bluetooth (esp_hosted over SDIO).
 *
 * Display, touch, SD card, audio codec and the IO expanders come from
 * Espressif's m5stack_tab5 BSP (pins in bsp/m5stack_tab5.h), as in
 * esp32_basic and wmbus_hub.
 */
#pragma once

#include "driver/gpio.h"

#define BOARD_NAME              "M5Stack Tab5"
#define BOARD_SHORT_NAME        "Tab5"

/* ---- LCD: 5" 720x1280 MIPI-DSI panel (ILI9881C, ST7123 or ST7121), held
 * landscape: the runtime draws a 1280x720 picture, lcd_flush() turns it
 * onto the panel (PPA). Landscape (x, y) is panel (y, 1279 - x), the
 * orientation esp32_basic uses with the Tab5 Keyboard below the screen.
 */
#define BOARD_LCD_H_RES         1280
#define BOARD_LCD_V_RES         720
#define BOARD_PANEL_W           720
#define BOARD_PANEL_H           1280
#define BOARD_LCD_BRIGHTNESS    80      /* % */

/* ---- Speaker: ES8388 codec + NS4150 amplifier (BSP); the USB port stays */
#define BOARD_SPEAKER_TAKES_USB 0

/* ---- ESP32-C6: power on PI4IOE5V6408 0x44 pin 0, SDIO slot 1 (pins in
 * the esp_hosted settings, ../boards/m5stack_tab5/sdkconfig.defaults)
 */
#define BOARD_C6_EXPANDER_ADDR  0x44
#define BOARD_C6_BOOT_MS        2000    /* from power on to its SDIO slave */

/* ---- Battery charger IP2326: CHG_EN on IO expander 0x44 pin 7 ---- */

/* ---- Tab5 Keyboard (optional): Ext.Port1 I2C; the BSP has I2C_NUM_1 ---- */
#define BOARD_KBD_I2C_PORT      I2C_NUM_0
#define BOARD_KBD_GPIO_SDA      GPIO_NUM_0
#define BOARD_KBD_GPIO_SCL      GPIO_NUM_1
#define BOARD_KBD_ADDR          0x6D
