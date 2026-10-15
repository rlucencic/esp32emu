#pragma once

/*
 * The emulator runtime on the Makerfabs MaTouch ESP32-S3 7": a machine
 * (Atari STE, Macintosh, ...) describes itself in an emu_machine_t, and
 * emu_run() does the rest:
 *
 *   core 1: the emulation task, one machine frame at a time, paced by the
 *           clock (or by the speaker's I2S clock with CONFIG_EMU_SPEAKER)
 *   core 0: presenter (machine screen scaled onto the LCD when it changed),
 *           serial console, stats (1 s), the supervisor (2 s hold on the
 *           top-right corner opens the setup screen), WiFi, NimBLE, web
 *
 * Boot order: NVS, the machine's memory (first: big blocks fragment
 * later), SD card and disks, LCD, touch, console, WiFi (before BLE: it
 * needs the bigger internal RAM blocks), web, BLE, setup screen, start.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "emu/disks.h"
#include "emu/json.h"

typedef struct {
    int width;
    int height;
    int hz;                     /* frames per second of the emulated video */
} emu_screen_t;

typedef enum {
    EMU_SCALE_NEAREST,          /* nearest pixel (colour pictures) */
    EMU_SCALE_SMOOTH,           /* filtered (black and white at odd factors) */
} emu_scale_t;

/* A row of choices on the setup screen: "System   [TOS 1.62] [TOS 2.06]".
 * Changing one while the machine runs makes "Start" a "Restart".
 */
typedef struct {
    const char *label;
    const char *const *names;   /* button labels, ends with "" */
    int (*get)(void);
    void (*set)(int index);     /* also saves it */
} emu_choice_t;

typedef struct {
    const char *name;           /* "Atari STE": titles, web page, mDNS */
    const char *short_name;     /* "Atari": "Start Atari", "Reset Atari" */
    int view_width;             /* picture width on the 1024 x 600 panel */
    emu_scale_t scale;

    /* Power-on, before anything else takes PSRAM */
    void (*reserve_memory)(void);
    /* Right after the SD card was mounted (or wasn't) */
    void (*storage)(bool sd_card);
    /* (Re)start with the current settings; false if it can't */
    bool (*start)(void);
    void (*reset)(void);        /* cold reset (emulation paused) */
    void (*run_frame)(void);
    bool (*halted)(void);       /* the CPU stopped until a reset */

    void (*screen_mode)(emu_screen_t *mode);
    uint32_t (*screen_signature)(void);     /* changes when the picture does */
    void (*render_line)(int y, uint16_t *dst);      /* RGB565, mode.width pixels */

    /* Sound, 16-bit stereo (render NULL: none) */
    int audio_hz;
    void (*audio_enable)(bool on);
    int (*audio_render)(int16_t *dst, int frames);
    int (*audio_pending)(void);

    /* Input, from other tasks */
    void (*hid_key)(uint8_t usage, bool pressed);   /* HID usage, $E0-$E7 modifiers */
    void (*hid_mouse)(int dx, int dy, uint8_t buttons);
    /* Touch on the picture, in machine screen pixels (NULL: not used) */
    void (*touch)(int x, int y, bool pressed);

    /* Setup screen */
    const emu_choice_t *choices;
    int choice_count;

    /* Disk images on the SD card for the machine's drives (NULL: none) */
    const disks_config_t *disks;
    /* JavaScript for the web page, served as /machine.js (NULL: none): it can
     * call registerConverter({description, extensions, convert}) so that
     * dropped files with those extensions are turned into disk images in the
     * browser (see web/index.html). Usually an EMBED_FILES start and end.
     */
    const char *web_script, *web_script_end;

    /* Status for the web page: EMU_STATUS_ROWS adds the machine card's rows
     * (emu_json_row()), EMU_STATUS_DRIVES its drives (emu_json_drive())
     */
    void (*status)(emu_json_t *j, int section);
    void (*console_info)(char *buf, size_t len);
    /* Console lines the runtime doesn't know (e.g. 'R'); true if handled */
    bool (*console_command)(const char *line);
    /* Once a second from a core 0 task (e.g. save a setting) */
    void (*every_second)(void);
} emu_machine_t;

enum { EMU_STATUS_ROWS, EMU_STATUS_DRIVES };

/* Never returns to the caller's work: everything runs in tasks afterwards */
void emu_run(const emu_machine_t *machine);

const emu_machine_t *emu_machine(void);

typedef enum {
    EMU_SETUP,                  /* setup screen before the first start */
    EMU_RUNNING,
    EMU_PAUSED,                 /* setup screen while the machine waits */
    EMU_HALTED,                 /* the CPU stopped until a reset */
} emu_state_t;

typedef struct {
    emu_state_t state;
    float fps;                  /* frames emulated per second, last second */
    int speed;                  /* % of real speed */
    float load[2];              /* % busy, cores 0 and 1, last second */
    float temperature;          /* chip, degrees C, as last measured; NAN if never */
    uint32_t session_s;         /* seconds since the machine was (re)started */
    bool sd_card;
} emu_info_t;

/* Updated about once a second; safe from any task */
void emu_info(emu_info_t *info);

/* Have the chip temperature measured (at most every 5 s) */
void emu_measure_temperature(void);

/* Cold reset from the emulation task */
void emu_request_reset(void);

/* Run fn with the emulation stopped between frames (or right away when it
 * isn't running). False if the setup screen holds it.
 */
bool emu_paused(void (*fn)(void *arg), void *arg);

/* Start the machine again with its current settings (paused meanwhile) */
bool emu_restart(void);

/* Restart the board and go straight into the machine (for memory that only
 * fits in one block right after power-on)
 */
void emu_reboot_into_machine(void);

/* Text for the setup screen: what the SD card is used for (by default
 * "Disk images on the SD card" or "No SD card")
 */
void emu_set_drive_info(const char *text);

/* Why the machine can't start (e.g. "No ROM"), shown on the setup screen;
 * NULL clears it
 */
void emu_set_message(const char *text);
