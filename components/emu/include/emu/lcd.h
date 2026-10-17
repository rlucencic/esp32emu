#pragma once

#include <stdint.h>
#include "sdkconfig.h"

/* Bring up the panel and backlight. Returns the RGB565 picture
 * (BOARD_LCD_H_RES x BOARD_LCD_V_RES, PSRAM) or NULL on failure.
 * MaTouch: the RGB panel's own framebuffer, pixels written there show up on
 * the next refresh. Tab5: a landscape copy that lcd_flush() puts onto the
 * portrait panel.
 */
uint16_t *lcd_init(void);

/* Rows y0 .. y1 - 1 of the picture changed */
#if CONFIG_IDF_TARGET_ESP32P4
void lcd_flush(int y0, int y1);
#else
static inline void lcd_flush(int y0, int y1)
{
}
#endif
