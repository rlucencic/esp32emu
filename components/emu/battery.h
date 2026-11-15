/*
 * The board's battery, on boards that have one (BOARD_HAS_BATTERY: the
 * Tab5, tab5.c); shown on the setup screen's status lines and in the web
 * page's status
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BATTERY_NONE,           /* no pack, or one too flat to count */
    BATTERY_DISCHARGING,    /* the board runs from it */
    BATTERY_IDLE,           /* external power, no current either way (full, or not charging) */
    BATTERY_CHARGING,
} battery_state_t;

typedef struct {
    battery_state_t state;
    int mv;                 /* the pack's voltage */
    int ma;                 /* its current: + charging, - discharging */
    int percent;            /* 0-100 from the voltage (higher while charging) */
    /* the charger's lines as read back from the chip that drives them
     * (-1: couldn't be read)
     */
    int charger;            /* 1: switched on (its enable line driven high) */
    bool charger_driven;    /* (the same: an output, not high-Z, set high) */
    int charger_status;     /* its status line's level (an input; high whether it charges or not) */
    int charger_fast;       /* 1: the 1 A step (its QC line driven low), 0: 500 mA */
    uint8_t charger_regs[6];    /* the expander's direction, output, high-Z, pull enable, pull select, input */
} battery_t;

/* The battery now: false if it can't be read */
bool battery_read(battery_t *b);

/* "charging", "discharging", "idle", "none" */
const char *battery_state_name(battery_state_t state);
