/*
 * The board, chosen by the build target:
 *   esp32s3  Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (board_matouch7.h)
 *   esp32p4  M5Stack Tab5 (board_tab5.h)
 *
 * Both give BOARD_NAME, BOARD_SHORT_NAME and BOARD_LCD_H_RES x
 * BOARD_LCD_V_RES, the landscape picture the runtime draws (lcd.h).
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "emu/board_tab5.h"
#else
#include "emu/board_matouch7.h"
#endif
