/*
 * Between the runtime's own modules. Not for machines.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "emu/emu.h"

/* emu.c */
const char *emu_drive_info(void);
const char *emu_message(void);          /* why the machine didn't start, "" if it did */
bool emu_started(void);

/* setup_ui.c */
#define EMU_UI_MAX_CHOICES  6

typedef struct {
    bool reset;                         /* [Reset] pressed */
    bool changed;                       /* a choice differs from when the screen opened */
    int before[EMU_UI_MAX_CHOICES];     /* the choices then */
} emu_ui_result_t;

/* The setup screen in the LCD framebuffer; returns once the user starts
 * (or, with `running`, goes back to) the machine. LVGL is shut down again.
 */
void emu_ui_run(uint16_t *fb, bool running, emu_ui_result_t *result);

/* console.c, web.c */
void emu_console_start(uint16_t *lcd_fb);
void emu_web_start(void);
