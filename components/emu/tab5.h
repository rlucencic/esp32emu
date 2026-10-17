/*
 * M5Stack Tab5 board glue (tab5.c, tab5_keyboard.c), called from emu.c
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* First thing at boot: I2C, battery charging, power to the ESP32-C6 */
void tab5_init(void);

/* WiFi through the C6 (esp_hosted over SDIO): before net_init() */
bool tab5_radio_start(void);

/* The C6's Bluetooth controller: before ble_hid_start() */
bool tab5_bt_start(void);

/* The Tab5 Keyboard on Ext.Port1, if there is one: its keys as HID usages
 * (modifiers $E0-$E7), like a Bluetooth keyboard's
 */
bool tab5_keyboard_start(void (*key)(uint8_t usage, bool pressed));
