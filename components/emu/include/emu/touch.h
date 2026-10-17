#pragma once

#include <stdbool.h>

/* The touch panel. Call after lcd_init() (MaTouch: it pulses the shared
 * reset; Tab5: the BSP has found out which panel it is)
 */
bool touch_init(void);

/* First touch point in picture pixels (BOARD_LCD_H_RES x BOARD_LCD_V_RES);
 * false if no touch panel
 */
bool touch_read(int *x, int *y, bool *pressed);

/* Pretend a finger is at (x, y) for `ms` (console 't': remote testing) */
void touch_inject(int x, int y, int ms);
