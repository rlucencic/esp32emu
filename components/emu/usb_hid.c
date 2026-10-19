/*
 * M5Stack Tab5: USB keyboards and mice on its USB-A port, as the machine's
 * (besides Bluetooth ones and the Tab5 Keyboard). The BSP's USB host (the
 * port's 5 V, the USB host library and its task) and espressif/usb_host_hid
 * in boot protocol: keyboards' 8-byte reports [modifiers, 0, 6 usages] and
 * mice's [buttons, dx, dy (, wheel)], decoded as the Bluetooth ones are. A
 * receiver for both (an interface each) is both; up to four at once.
 */
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "emu_internal.h"

#ifdef CONFIG_EMU_MOUSE_DIVIDER
#define MOUSE_DIVIDER       CONFIG_EMU_MOUSE_DIVIDER
#else
#define MOUSE_DIVIDER       2
#endif
#define SLOTS               4

static const char *TAG = "usb_hid";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} device_event_t;

static QueueHandle_t s_events;
static void (*s_key)(uint8_t usage, bool pressed);
static void (*s_mouse)(int dx, int dy, uint8_t buttons);

static struct {
    hid_host_device_handle_t handle;    /* NULL: free */
    uint8_t prev[8];                    /* a keyboard's last report */
    int rem_x, rem_y;                   /* a mouse's motion below the divider */
} s_slots[SLOTS];
static volatile int s_connected;

static int slot_for(hid_host_device_handle_t h, bool add)
{
    for (int i = 0; i < SLOTS; i++) {
        if (s_slots[i].handle == h) {
            return i;
        }
    }
    if (!add) {
        return -1;
    }
    for (int i = 0; i < SLOTS; i++) {
        if (s_slots[i].handle == NULL) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].handle = h;
            return i;
        }
    }
    return -1;
}

static bool in_report(const uint8_t *r, uint8_t usage)
{
    for (int i = 2; i < 8; i++) {
        if (r[i] == usage) {
            return true;
        }
    }
    return false;
}

/* Key edges between two reports: modifiers as usages $E0-$E7 */
static void keyboard_edges(const uint8_t *old, const uint8_t *now)
{
    for (int b = 0; b < 8; b++) {
        uint8_t m = (uint8_t)(1u << b);
        if ((old[0] ^ now[0]) & m) {
            s_key((uint8_t)(0xE0 + b), (now[0] & m) != 0);
        }
    }
    for (int i = 2; i < 8; i++) {
        if (old[i] > 0x03 && !in_report(now, old[i])) {
            s_key(old[i], false);
        }
    }
    for (int i = 2; i < 8; i++) {
        if (now[i] > 0x03 && !in_report(old, now[i])) {
            s_key(now[i], true);
        }
    }
}

static void interface_event(hid_host_device_handle_t h, const hid_host_interface_event_t event, void *arg)
{
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK) {
        return;
    }
    int slot = slot_for(h, false);
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (slot < 0 || hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK) {
            return;
        }
        if (p.proto == HID_PROTOCOL_KEYBOARD && len >= 8) {
            if (data[2] == 0x01) {
                return;                 /* phantom state: too many keys down */
            }
            keyboard_edges(s_slots[slot].prev, data);
            memcpy(s_slots[slot].prev, data, 8);
        } else if (p.proto == HID_PROTOCOL_MOUSE && len >= 3) {
            int dx = (int8_t)data[1] + s_slots[slot].rem_x;
            int dy = (int8_t)data[2] + s_slots[slot].rem_y;
            s_slots[slot].rem_x = dx % MOUSE_DIVIDER;
            s_slots[slot].rem_y = dy % MOUSE_DIVIDER;
            if (s_mouse) {
                s_mouse(dx / MOUSE_DIVIDER, dy / MOUSE_DIVIDER, data[0] & 0x07);
            }
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (slot >= 0) {
            /* let go of its keys */
            static const uint8_t none[8];
            if (p.proto == HID_PROTOCOL_KEYBOARD) {
                keyboard_edges(s_slots[slot].prev, none);
            }
            s_slots[slot].handle = NULL;
        }
        s_connected--;
        ESP_LOGI(TAG, "%s disconnected", p.proto == HID_PROTOCOL_MOUSE ? "mouse" : "keyboard");
        hid_host_device_close(h);
        break;
    default:
        break;
    }
}

/* From the HID driver's task: the rest in ours */
static void driver_event(hid_host_device_handle_t h, const hid_host_driver_event_t event, void *arg)
{
    const device_event_t e = { h, event };
    xQueueSend(s_events, &e, 0);
}

static void usb_hid_task(void *arg)
{
    device_event_t e;
    for (;;) {
        if (xQueueReceive(s_events, &e, portMAX_DELAY) != pdTRUE || e.event != HID_HOST_DRIVER_EVENT_CONNECTED) {
            continue;
        }
        hid_host_dev_params_t p;
        if (hid_host_device_get_params(e.handle, &p) != ESP_OK) {
            continue;
        }
        if (p.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
            (p.proto != HID_PROTOCOL_KEYBOARD && p.proto != HID_PROTOCOL_MOUSE)) {
            ESP_LOGI(TAG, "a HID interface without a boot protocol (subclass %d, protocol %d): not used",
                     p.sub_class, p.proto);
            continue;
        }
        if (slot_for(e.handle, true) < 0) {
            ESP_LOGW(TAG, "too many USB keyboards and mice");
            continue;
        }
        const hid_host_device_config_t config = { .callback = interface_event, .callback_arg = NULL };
        if (hid_host_device_open(e.handle, &config) != ESP_OK) {
            s_slots[slot_for(e.handle, false)].handle = NULL;
            continue;
        }
        hid_class_request_set_protocol(e.handle, HID_REPORT_PROTOCOL_BOOT);
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            hid_class_request_set_idle(e.handle, 0, 0);
        }
        if (hid_host_device_start(e.handle) == ESP_OK) {
            s_connected++;
            ESP_LOGI(TAG, "USB %s connected", p.proto == HID_PROTOCOL_MOUSE ? "mouse" : "keyboard");
        }
    }
}

bool usb_hid_start(void (*key)(uint8_t usage, bool pressed), void (*mouse)(int dx, int dy, uint8_t buttons))
{
    s_key = key;
    s_mouse = mouse;
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no USB host: %s", esp_err_to_name(err));
        return false;
    }
    s_events = xQueueCreate(8, sizeof(device_event_t));
    const hid_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_event,
        .callback_arg = NULL,
    };
    err = hid_host_install(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HID host: %s", esp_err_to_name(err));
        return false;
    }
    xTaskCreatePinnedToCore(usb_hid_task, "usb_hid", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "USB keyboards and mice on the USB-A port");
    return true;
}

int usb_hid_connected(void)
{
    return s_connected;
}
