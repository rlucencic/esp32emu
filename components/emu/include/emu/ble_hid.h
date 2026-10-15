#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * BLE HID host (NimBLE central): a keyboard and a mouse at the same time.
 * Paired devices reconnect by themselves; new ones are paired from the
 * setup screen.
 *
 * Reports arrive in the NimBLE host task: keys as HID usages (letters
 * $04.., modifiers $E0-$E7), mouse motion already divided by
 * CONFIG_EMU_MOUSE_DIVIDER with buttons bit 0 left, 1 right, 2 middle.
 * When a device disconnects, its keys are released.
 *
 * Needs nvs_flash_init() first (pairings are stored in NVS).
 */
typedef struct {
    void (*key)(uint8_t usage, bool pressed);
    void (*mouse)(int dx, int dy, uint8_t buttons);
} ble_hid_callbacks_t;

bool ble_hid_start(const ble_hid_callbacks_t *callbacks);

enum { HID_KIND_UNKNOWN = 0, HID_KIND_KEYBOARD, HID_KIND_MOUSE };

typedef struct {
    char name[32];
    uint8_t kind;           /* HID_KIND_* */
    int8_t rssi;            /* candidates only */
    bool connected;         /* paired devices only */
    int8_t battery;         /* connected paired devices: %, -1 = unknown */
} ble_hid_device_t;

/* Paired devices */
int ble_hid_devices(ble_hid_device_t *out, int max);

/* Pairing mode: list HID devices that are in pairing mode nearby */
void ble_hid_set_pairing(bool on);
int ble_hid_candidates(ble_hid_device_t *out, int max);
void ble_hid_pair(int index);

/* Remove a paired device (disconnects it and deletes the bond) */
void ble_hid_forget(int index);

/* Last pairing/forgetting outcome for the screen, "" if none */
const char *ble_hid_status(void);
