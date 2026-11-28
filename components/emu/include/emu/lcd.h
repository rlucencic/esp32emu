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

/* Tab5: rows straight onto the panel, past the landscape copy (the
 * presenter's: a row written there, read back and turned cost about four
 * times the PSRAM traffic). n rows of w pixels, w apart, in internal RAM, at
 * (x, y) of the picture; x and w multiples of LCD_STRIP_COLS, y and n of
 * LCD_STRIP_ROWS (the PPA's tail, DIG-734). The copy keeps what it had: an
 * lcd_flush() of those rows puts that back. MaTouch: none (LCD_STRIPS 0).
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define LCD_STRIPS          1
#define LCD_STRIP_ROWS      8
#define LCD_STRIP_COLS      32
void lcd_put_strip(const uint16_t *pixels, int x, int w, int y, int n);
#else
#define LCD_STRIPS          0
#define LCD_STRIP_ROWS      8
#define LCD_STRIP_COLS      32
static inline void lcd_put_strip(const uint16_t *pixels, int x, int w, int y, int n)
{
}
#endif

/* Rows y .. y + n - 1 of what the panel shows, BOARD_LCD_H_RES pixels each,
 * landscape (the console's `lcd`)
 */
void lcd_read(int y, int n, uint16_t *dst);

/* MaTouch: how often the RGB panel's output got out of step with the
 * picture (refill interrupts lost to a flash write or a busy core 0) and
 * was put right at the next VSYNC (components/esp_lcd); 0 on the Tab5
 */
uint32_t lcd_resyncs(void);
