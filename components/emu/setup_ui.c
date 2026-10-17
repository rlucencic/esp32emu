/*
 * Setup screen (LVGL 9), shown before the machine starts:
 *
 *   status     WiFi and clock, the machine's choices (system, memory, ...),
 *              [WiFi] [Bluetooth] [Disks] [Start <machine>]
 *   networks   scan results; tap one to connect
 *   password   text field + on-screen keyboard
 *   bluetooth  paired keyboard/mouse, [Forget] each, [Pair new device]
 *   pairing    HID devices in pairing mode nearby; tap one to pair
 *   disks      the machine's drives with [Insert disk] [New empty disk] [Eject]
 *   images     disk images on the SD card; tap one to insert it
 *
 * It waits for [Start]. With CONFIG_EMU_AUTOSTART it also starts the
 * machine after a countdown once the clock is set (or without saved WiFi);
 * any touch stops the countdown. Long-pressing the top-right corner while
 * the machine runs brings this screen back ([Back] then).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "emu/ble_hid.h"
#include "emu/board.h"
#include "emu/disks.h"
#include "emu/lcd.h"
#include "emu/net.h"
#include "emu/touch.h"
#include "emu_internal.h"

#define BUF_LINES           60
#if CONFIG_EMU_AUTOSTART
#define AUTOSTART_S         CONFIG_EMU_AUTOSTART_SECONDS
#define OFFLINE_START_S     (CONFIG_EMU_AUTOSTART_SECONDS * 2)
#endif
#define MAX_LIST            16

/* The layout is for the 1024 x 600 MaTouch; other panels scale it */
#define SX(v)               ((v) * BOARD_LCD_H_RES / 1024)
#define SY(v)               ((v) * BOARD_LCD_V_RES / 600)
#define PAD                 SX(32)
#if BOARD_LCD_V_RES >= 720
#define FONT_BIG            lv_font_montserrat_34
#else
#define FONT_BIG            lv_font_montserrat_28
#endif

#define COLOR_BG            lv_color_hex(0x10141C)
#define COLOR_CARD          lv_color_hex(0x1E2533)
#define COLOR_TEXT          lv_color_hex(0xE8ECF2)
#define COLOR_DIM           lv_color_hex(0x8A94A6)
#define COLOR_OK            lv_color_hex(0x4CC38A)
#define COLOR_WARN          lv_color_hex(0xE5A54B)
#define COLOR_ROW           lv_color_hex(0x283143)
#define COLOR_ROW_PRESSED   lv_color_hex(0x34405A)

static struct {
    const emu_machine_t *m;
    const disks_config_t *disks;
    uint16_t *fb;
    bool running;                   /* the machine is paused behind this screen */
    int before[EMU_UI_MAX_CHOICES]; /* its choices when the screen opened */
    bool done;
    bool reset;                     /* [Reset] pressed */
    bool countdown_cancelled;
    int64_t countdown_end;          /* 0 = not counting */
    lv_obj_t *status_screen, *list_screen, *pass_screen, *bt_screen, *pair_screen;
    lv_obj_t *wifi_label, *time_label, *countdown_label, *start_label, *message_label;
    lv_obj_t *list, *scan_label;
    bool scan_pending;
    lv_obj_t *pass_title, *pass_field;
    char chosen_ssid[33];
    lv_obj_t *bt_list, *bt_status, *pair_list, *pair_status;
    char bt_shown[512], pair_shown[512];   /* what the lists show now */
    lv_obj_t *disk_screen, *images_screen;
    lv_obj_t *disk_status, *drive_label[DISKS_MAX_DRIVES], *drive_buttons[DISKS_MAX_DRIVES][3];
    lv_obj_t *images_title, *images_count, *images_list;
    int images_drive;                       /* drive the chooser inserts into */
    char **images;
    int image_count;
} ui;

/* ---- display + input glue -------------------------------------------------- */

static uint32_t tick_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int w = lv_area_get_width(area);
    const uint16_t *src = (const uint16_t *)px;
    for (int y = area->y1; y <= area->y2; y++, src += w) {
        memcpy(ui.fb + y * BOARD_LCD_H_RES + area->x1, src, w * sizeof(uint16_t));
    }
    lcd_flush(area->y1, area->y2 + 1);
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int x = 0, y = 0;
    bool pressed = false;
    touch_read(&x, &y, &pressed);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        ui.countdown_cancelled = true;
    }
}

/* ---- helpers ---------------------------------------------------------------- */

static lv_obj_t *make_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, COLOR_BG, 0);
    lv_obj_set_style_text_color(s, COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(s, PAD, 0);
    return s;
}

static lv_obj_t *make_button_for(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int width,
                                 void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, SX(width), SY(72));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int width)
{
    return make_button_for(parent, text, cb, width, NULL);
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_font(t, &FONT_BIG, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    return t;
}

static lv_obj_t *make_list(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *l = lv_list_create(parent);
    lv_obj_set_size(l, lv_pct(100), SY(height));
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, SY(y));
    lv_obj_set_style_bg_color(l, COLOR_CARD, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    return l;
}

/* A list row in the dark theme */
static lv_obj_t *add_row(lv_obj_t *list, const char *icon, const char *text)
{
    lv_obj_t *b = lv_list_add_button(list, icon, text);
    lv_obj_set_height(b, SY(64));
    lv_obj_set_style_bg_color(b, COLOR_ROW, 0);
    lv_obj_set_style_bg_color(b, COLOR_ROW_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, COLOR_TEXT, 0);
    lv_obj_set_style_border_color(b, COLOR_CARD, 0);
    return b;
}

/* A row "label  [a][b][c]" with one choice checked */
static lv_obj_t *make_choice(lv_obj_t *parent, const char *label, const char *const *names,
                             int checked, int height, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), SY(height));
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, COLOR_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *m = lv_buttonmatrix_create(row);
    lv_buttonmatrix_set_map(m, (const char **)names);
    lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
    lv_buttonmatrix_set_button_ctrl(m, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(m, SX(560), SY(height));
    lv_obj_align(m, LV_ALIGN_LEFT_MID, SX(160), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return m;
}

/* ---- status screen ---------------------------------------------------------- */

static void start_clicked(lv_event_t *e)
{
    ui.done = true;
}

static void reset_clicked(lv_event_t *e)
{
    ui.reset = true;
    ui.done = true;
}

static void back_to_status(lv_event_t *e)
{
    lv_screen_load(ui.status_screen);
}

/* Live choices change the machine as they're set: no restart for them */
static bool choices_changed(void)
{
    for (int i = 0; i < ui.m->choice_count && i < EMU_UI_MAX_CHOICES; i++) {
        if (!ui.m->choices[i].live && ui.m->choices[i].get() != ui.before[i]) {
            return true;
        }
    }
    return false;
}

/* Paused machine: [Back] resumes it, [Restart] when a choice changed */
static void update_start_label(void)
{
    const char *symbol = LV_SYMBOL_PLAY, *verb = "Start";
    if (ui.running) {
        bool changed = choices_changed();
        symbol = changed ? LV_SYMBOL_REFRESH : LV_SYMBOL_PLAY;
        verb = changed ? "Restart" : "Back";
    }
    lv_label_set_text_fmt(ui.start_label, "%s  %s", symbol, verb);
}

static void choice_changed(lv_event_t *e)
{
    const emu_choice_t *c = lv_event_get_user_data(e);
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i != LV_BUTTONMATRIX_BUTTON_NONE) {
        c->set(i);
        update_start_label();
    }
}

static void show_networks(lv_event_t *e);
static void show_disks(lv_event_t *e);
#if CONFIG_EMU_BLE_HID
static void show_bluetooth(lv_event_t *e);
#endif

static void build_status_screen(void)
{
    lv_obj_t *s = ui.status_screen = make_screen();
    char title[64];
    snprintf(title, sizeof(title), LV_SYMBOL_HOME "  %s", ui.m->name);
    make_title(s, title);

    lv_obj_t *card = lv_obj_create(s);
    lv_obj_set_size(card, lv_pct(100), SY(380));
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, SY(56));
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    /* Three rows of choices (and the lines around them) only fit closer */
    bool tight = ui.m->choice_count >= 3;
    lv_obj_set_style_pad_all(card, SY(24), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, SY(tight ? 10 : 16), 0);

    ui.wifi_label = lv_label_create(card);
    ui.time_label = lv_label_create(card);

    for (int i = 0; i < ui.m->choice_count && i < EMU_UI_MAX_CHOICES; i++) {
        const emu_choice_t *c = &ui.m->choices[i];
        make_choice(card, c->label, c->names, c->get(), tight ? 54 : 60, choice_changed, (void *)c);
    }

    ui.message_label = lv_label_create(card);
    lv_obj_set_width(ui.message_label, lv_pct(100));
    lv_label_set_long_mode(ui.message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui.message_label, COLOR_WARN, 0);
    lv_label_set_text(ui.message_label, emu_message());
    if (emu_message()[0] == 0) {
        lv_obj_add_flag(ui.message_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *info = lv_label_create(card);
    lv_label_set_text_fmt(info, "%s\n%s", emu_drive_info(),
                          ui.running ? "Changing these settings restarts the machine."
                                     : "Hold the top-right corner of the screen for 2 s to come back here.");
    lv_obj_set_style_text_color(info, COLOR_DIM, 0);

    lv_obj_t *wifi_btn = make_button(s, LV_SYMBOL_WIFI "  WiFi", show_networks, 190);
    lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(wifi_btn, COLOR_CARD, 0);
#if CONFIG_EMU_BLE_HID
    lv_obj_t *bt_btn = make_button(s, LV_SYMBOL_BLUETOOTH "  Bluetooth", show_bluetooth, 240);
    lv_obj_align(bt_btn, LV_ALIGN_BOTTOM_LEFT, SX(206), 0);
    lv_obj_set_style_bg_color(bt_btn, COLOR_CARD, 0);
#endif
    if (ui.disks != NULL) {
        char text[40];
        snprintf(text, sizeof(text), LV_SYMBOL_SAVE "  %s", ui.disks->button);
        lv_obj_t *disk_btn = make_button(s, text, show_disks, 200);
        lv_obj_align(disk_btn, LV_ALIGN_BOTTOM_LEFT, SX(462), 0);
        lv_obj_set_style_bg_color(disk_btn, COLOR_CARD, 0);
    }

    lv_obj_t *start_btn = make_button(s, "", start_clicked, 280);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    ui.start_label = lv_obj_get_child(start_btn, 0);
    update_start_label();

    if (ui.running) {
        lv_obj_t *reset = make_button(s, LV_SYMBOL_REFRESH "  Reset", reset_clicked, 160);
        lv_obj_set_height(reset, SY(48));
        lv_obj_align(reset, LV_ALIGN_TOP_RIGHT, 0, -SY(4));
        lv_obj_set_style_bg_color(reset, COLOR_CARD, 0);
    }

    ui.countdown_label = lv_label_create(s);
    lv_obj_set_style_text_color(ui.countdown_label, COLOR_DIM, 0);
    lv_obj_align(ui.countdown_label, LV_ALIGN_TOP_RIGHT, 0, SY(6));
    lv_label_set_text(ui.countdown_label, "");
}

static void update_status(void)
{
    char ip[20];
    switch (net_state()) {
    case NET_OFF:
        lv_label_set_text(ui.wifi_label, LV_SYMBOL_WIFI "  WiFi: not set up");
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_WARN, 0);
        break;
    case NET_CONNECTING:
        lv_label_set_text_fmt(ui.wifi_label, LV_SYMBOL_WIFI "  WiFi: connecting to %s ...", net_ssid());
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_TEXT, 0);
        break;
    case NET_CONNECTED:
        net_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(ui.wifi_label, LV_SYMBOL_WIFI "  WiFi: %s  (%s)", net_ssid(), ip);
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_OK, 0);
        break;
    case NET_FAILED:
        lv_label_set_text_fmt(ui.wifi_label, LV_SYMBOL_WARNING "  WiFi: can't connect to %s (retrying)",
                              net_ssid());
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_WARN, 0);
        break;
    }

    if (net_time_source() != NET_TIME_NONE) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d  %H:%M:%S", &tm);
        lv_label_set_text_fmt(ui.time_label, LV_SYMBOL_REFRESH "  Time: %s", buf);
        lv_obj_set_style_text_color(ui.time_label, COLOR_OK, 0);
    } else {
        lv_label_set_text(ui.time_label, LV_SYMBOL_REFRESH "  Time: waiting for the internet");
        lv_obj_set_style_text_color(ui.time_label, COLOR_DIM, 0);
    }

#if CONFIG_EMU_AUTOSTART
    /* Countdown: once the clock is set, or soon without any WiFi set up;
     * not when the machine couldn't start last time
     */
    int64_t now_us = esp_timer_get_time();
    if (ui.running || ui.countdown_cancelled || emu_message()[0]) {
        ui.countdown_end = 0;
        lv_label_set_text(ui.countdown_label, "");
    } else if (ui.countdown_end == 0) {
        if (net_time_source() != NET_TIME_NONE) {
            ui.countdown_end = now_us + AUTOSTART_S * 1000000LL;
        } else if (net_state() == NET_OFF) {
            ui.countdown_end = now_us + OFFLINE_START_S * 1000000LL;
        }
    }
    if (ui.countdown_end) {
        int left = (ui.countdown_end - now_us + 999999) / 1000000;
        if (left <= 0) {
            ui.done = true;
        } else {
            lv_label_set_text_fmt(ui.countdown_label, "Starting in %d s - touch to stay", left);
        }
    }
#endif
}

/* ---- WiFi networks ------------------------------------------------------------ */

static void back_from_networks(lv_event_t *e)
{
    net_resume();
    lv_screen_load(ui.status_screen);
}

static void scan_clicked(lv_event_t *e)
{
    lv_obj_clean(ui.list);
    lv_label_set_text(ui.scan_label, "Scanning ...");
    ui.scan_pending = true;
    net_scan_start();
}

static void network_clicked(lv_event_t *e)
{
    const net_ap_t *ap = lv_event_get_user_data(e);
    snprintf(ui.chosen_ssid, sizeof(ui.chosen_ssid), "%s", ap->ssid);
    if (!ap->secure) {
        net_connect(ui.chosen_ssid, "");
        lv_screen_load(ui.status_screen);
        return;
    }
    lv_label_set_text_fmt(ui.pass_title, "Password for %s", ui.chosen_ssid);
    lv_textarea_set_text(ui.pass_field, "");
    lv_screen_load(ui.pass_screen);
}

static void show_networks(lv_event_t *e)
{
    lv_screen_load(ui.list_screen);
    scan_clicked(NULL);
}

static void build_list_screen(void)
{
    lv_obj_t *s = ui.list_screen = make_screen();
    make_title(s, "Choose a network");
    ui.scan_label = lv_label_create(s);
    lv_obj_set_style_text_color(ui.scan_label, COLOR_DIM, 0);
    lv_obj_align(ui.scan_label, LV_ALIGN_TOP_RIGHT, 0, SY(6));
    ui.list = make_list(s, 56, 340);

    lv_obj_t *back = make_button(s, LV_SYMBOL_LEFT "  Back", back_from_networks, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
    lv_obj_t *rescan = make_button(s, LV_SYMBOL_REFRESH "  Scan again", scan_clicked, 300);
    lv_obj_align(rescan, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void update_networks(void)
{
    static net_ap_t aps[MAX_LIST];
    if (!ui.scan_pending || !net_scan_done()) {
        return;
    }
    ui.scan_pending = false;
    int n = net_scan_results(aps, MAX_LIST);
    lv_obj_clean(ui.list);
    lv_label_set_text_fmt(ui.scan_label, "%d found", n);
    for (int i = 0; i < n; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%.32s    %d dBm", aps[i].ssid, aps[i].rssi);
        lv_obj_t *b = add_row(ui.list, aps[i].secure ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI, text);
        lv_obj_add_event_cb(b, network_clicked, LV_EVENT_CLICKED, &aps[i]);
    }
}

/* ---- password ------------------------------------------------------------------ */

static void keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        net_connect(ui.chosen_ssid, lv_textarea_get_text(ui.pass_field));
        ui.countdown_end = 0;
        ui.countdown_cancelled = false;
        lv_screen_load(ui.status_screen);
    } else if (code == LV_EVENT_CANCEL) {
        lv_screen_load(ui.list_screen);
        scan_clicked(NULL);
    }
}

static void build_pass_screen(void)
{
    lv_obj_t *s = ui.pass_screen = make_screen();
    ui.pass_title = make_title(s, "");

    ui.pass_field = lv_textarea_create(s);
    lv_textarea_set_one_line(ui.pass_field, true);
    lv_textarea_set_placeholder_text(ui.pass_field, "password");
    lv_obj_set_width(ui.pass_field, lv_pct(100));
    lv_obj_align(ui.pass_field, LV_ALIGN_TOP_LEFT, 0, SY(56));

    lv_obj_t *kb = lv_keyboard_create(s);
    lv_obj_set_size(kb, BOARD_LCD_H_RES - 2 * PAD, SY(330));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ui.pass_field);
    lv_obj_add_event_cb(kb, keyboard_event, LV_EVENT_ALL, NULL);
}

/* ---- Bluetooth ----------------------------------------------------------------- */

#if CONFIG_EMU_BLE_HID

static const char *kind_symbol(uint8_t kind)
{
    return kind == HID_KIND_KEYBOARD ? LV_SYMBOL_KEYBOARD :
           kind == HID_KIND_MOUSE ? LV_SYMBOL_GPS : LV_SYMBOL_BLUETOOTH;
}

static void forget_clicked(lv_event_t *e)
{
    ble_hid_forget((int)(intptr_t)lv_event_get_user_data(e));
    ui.bt_shown[0] = 0;
}

static void pair_clicked(lv_event_t *e)
{
    ble_hid_pair((int)(intptr_t)lv_event_get_user_data(e));
}

static void show_bluetooth(lv_event_t *e)
{
    ble_hid_set_pairing(false);
    ui.bt_shown[0] = 0;
    lv_screen_load(ui.bt_screen);
}

static void show_pairing(lv_event_t *e)
{
    ble_hid_set_pairing(true);
    ui.pair_shown[0] = 0;
    lv_screen_load(ui.pair_screen);
}

static void build_bt_screens(void)
{
    lv_obj_t *s = ui.bt_screen = make_screen();
    make_title(s, "Bluetooth keyboard and mouse");
    ui.bt_status = lv_label_create(s);
    lv_obj_set_style_text_color(ui.bt_status, COLOR_DIM, 0);
    lv_obj_align(ui.bt_status, LV_ALIGN_TOP_LEFT, 0, SY(44));
    ui.bt_list = make_list(s, 84, 320);
    lv_obj_t *back = make_button(s, LV_SYMBOL_LEFT "  Back", back_to_status, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
    lv_obj_t *pair = make_button(s, LV_SYMBOL_PLUS "  Pair new device", show_pairing, 340);
    lv_obj_align(pair, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    s = ui.pair_screen = make_screen();
    make_title(s, "Pair a device");
    ui.pair_status = lv_label_create(s);
    lv_obj_set_style_text_color(ui.pair_status, COLOR_DIM, 0);
    lv_obj_align(ui.pair_status, LV_ALIGN_TOP_LEFT, 0, SY(44));
    ui.pair_list = make_list(s, 84, 320);
    lv_obj_t *done = make_button(s, LV_SYMBOL_LEFT "  Done", show_bluetooth, 300);
    lv_obj_align(done, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(done, COLOR_CARD, 0);
}

static void update_bluetooth(void)
{
    ble_hid_device_t dev[MAX_LIST];
    char shown[512];
    lv_obj_t *active = lv_screen_active();

    if (active == ui.bt_screen) {
        int n = ble_hid_devices(dev, MAX_LIST);
        int len = 0;
        for (int i = 0; i < n && len < (int)sizeof(shown) - 40; i++) {
            len += snprintf(shown + len, sizeof(shown) - len, "%s%d%d|", dev[i].name, dev[i].connected,
                            dev[i].battery);
        }
        shown[len] = 0;
        const char *status = ble_hid_status();
        lv_label_set_text(ui.bt_status, status[0] ? status :
                          n ? "Paired devices reconnect by themselves when switched on."
                            : "No keyboard or mouse paired yet.");
        if (strcmp(shown, ui.bt_shown) == 0) {
            return;
        }
        strcpy(ui.bt_shown, shown);
        lv_obj_clean(ui.bt_list);
        for (int i = 0; i < n; i++) {
            char text[80], battery[16] = "";
            if (dev[i].battery >= 0) {
                snprintf(battery, sizeof(battery), ", battery %d%%", dev[i].battery);
            }
            snprintf(text, sizeof(text), "%.31s  -  %s%s", dev[i].name,
                     dev[i].connected ? "connected" : "not connected", battery);
            lv_obj_t *row = add_row(ui.bt_list, kind_symbol(dev[i].kind), text);
            lv_obj_t *f = lv_button_create(row);
            lv_obj_set_size(f, SX(150), SY(48));
            lv_obj_add_event_cb(f, forget_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = lv_label_create(f);
            lv_label_set_text(l, LV_SYMBOL_TRASH "  Forget");
            lv_obj_center(l);
        }
    } else if (active == ui.pair_screen) {
        int n = ble_hid_candidates(dev, MAX_LIST);
        int len = 0;
        for (int i = 0; i < n && len < (int)sizeof(shown) - 40; i++) {
            len += snprintf(shown + len, sizeof(shown) - len, "%s%d|", dev[i].name, dev[i].kind);
        }
        shown[len] = 0;
        const char *status = ble_hid_status();
        lv_label_set_text(ui.pair_status, status[0] ? status :
                          "Put your keyboard or mouse into pairing mode, then tap it below.");
        if (strcmp(shown, ui.pair_shown) == 0) {
            return;
        }
        strcpy(ui.pair_shown, shown);
        lv_obj_clean(ui.pair_list);
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = add_row(ui.pair_list, kind_symbol(dev[i].kind), dev[i].name);
            lv_obj_add_event_cb(b, pair_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
}

#endif

/* ---- disks ---------------------------------------------------------------------- */

enum { BTN_INSERT, BTN_NEW, BTN_EJECT };

static void update_disks(void)
{
    for (int d = 0; d < ui.disks->drives; d++) {
        const char *name = disks_name(d);
        const char *drive = ui.disks->drive_names[d];
        if (name != NULL) {
            lv_label_set_text_fmt(ui.drive_label[d], "%s   %s%s", drive, name,
                                  ui.disks->write_protected(d) ? "   (read-only)" : "");
            lv_obj_set_style_text_color(ui.drive_label[d], COLOR_TEXT, 0);
        } else {
            lv_label_set_text_fmt(ui.drive_label[d], "%s   no disk", drive);
            lv_obj_set_style_text_color(ui.drive_label[d], COLOR_DIM, 0);
        }
        for (int b = 0; b < 3; b++) {
            bool usable = disks_ready() && (b != BTN_EJECT || name != NULL) &&
                          (b != BTN_NEW || (ui.disks->create != NULL &&
                                            (ui.disks->takes_blank == NULL || ui.disks->takes_blank(d))));
            if (usable) {
                lv_obj_remove_state(ui.drive_buttons[d][b], LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(ui.drive_buttons[d][b], LV_STATE_DISABLED);
            }
        }
    }
}

static void set_disk_status(const char *text, lv_color_t color)
{
    lv_label_set_text(ui.disk_status, text);
    lv_obj_set_style_text_color(ui.disk_status, color, 0);
}

static void show_disks(lv_event_t *e)
{
    if (disks_ready()) {
        char text[160];
        snprintf(text, sizeof(text), "%s from the SD card's \"%s\" folder.",
                 ui.disks->description, ui.disks->folder);
        set_disk_status(text, COLOR_DIM);
    } else {
        set_disk_status("No SD card: no disk images.", COLOR_WARN);
    }
    update_disks();
    lv_screen_load(ui.disk_screen);
}

static void image_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int d = ui.images_drive;
    const char *name = ui.images[i];
    char text[160];
    if (disks_insert(d, name)) {
        snprintf(text, sizeof(text), "%.100s is now in %s", name, ui.disks->drive_names[d]);
        set_disk_status(text, COLOR_OK);
    } else {
        snprintf(text, sizeof(text), "%.100s is not a disk image the %s can use.", name,
                 ui.m->short_name);
        set_disk_status(text, COLOR_WARN);
    }
    update_disks();
    lv_screen_load(ui.disk_screen);
}

static void insert_clicked(lv_event_t *e)
{
    int d = ui.images_drive = (int)(intptr_t)lv_event_get_user_data(e);
    disks_free_list(ui.images, ui.image_count);
    ui.image_count = disks_list(&ui.images);
    if (ui.disks->accepts != NULL) {
        /* only the images this drive takes */
        int kept = 0;
        for (int i = 0; i < ui.image_count; i++) {
            if (ui.disks->accepts(d, ui.images[i])) {
                ui.images[kept++] = ui.images[i];
            } else {
                free(ui.images[i]);
            }
        }
        ui.image_count = kept;
    }

    lv_label_set_text_fmt(ui.images_title, "Insert into %s", ui.disks->drive_names[d]);
    lv_label_set_text_fmt(ui.images_count, "%d disk%s", ui.image_count,
                          ui.image_count == 1 ? "" : "s");
    lv_obj_clean(ui.images_list);
    for (int i = 0; i < ui.image_count; i++) {
        const char *name = ui.images[i];
        char text[128];
        int in = -1;
        for (int k = 0; k < ui.disks->drives; k++) {
            if (disks_name(k) != NULL && strcmp(disks_name(k), name) == 0) {
                in = k;
            }
        }
        if (in >= 0) {
            snprintf(text, sizeof(text), "%.100s   (in %s)", name, ui.disks->drive_names[in]);
        } else {
            snprintf(text, sizeof(text), "%.100s", name);
        }
        lv_obj_t *b = add_row(ui.images_list, LV_SYMBOL_SAVE, text);
        lv_obj_add_event_cb(b, image_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (ui.image_count == 0) {
        lv_obj_t *l = lv_label_create(ui.images_list);
        lv_label_set_text_fmt(l, "No disk images yet. Copy them into the \"%s\" folder on the SD card,\n"
                                 "or make a new empty disk.", ui.disks->folder);
        lv_obj_set_style_text_color(l, COLOR_DIM, 0);
        lv_obj_set_style_pad_all(l, 16, 0);
    }
    lv_screen_load(ui.images_screen);
}

static void new_clicked(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    char name[32], text[96];
    set_disk_status("Making a new disk ...", COLOR_DIM);
    lv_refr_now(NULL);
    uint32_t bytes = disks_default_size(d);
    if (disks_create(d, bytes, name, sizeof(name))) {
        snprintf(text, sizeof(text), "New empty disk %s is now in %s", name, ui.disks->drive_names[d]);
        set_disk_status(text, COLOR_OK);
    } else {
        set_disk_status("Couldn't make a new disk on the SD card.", COLOR_WARN);
    }
    update_disks();
}

static void eject_clicked(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    disks_eject(d);
    char text[48];
    snprintf(text, sizeof(text), "%s is now empty", ui.disks->drive_names[d]);
    set_disk_status(text, COLOR_DIM);
    update_disks();
}

static void build_disk_screens(void)
{
    lv_obj_t *s = ui.disk_screen = make_screen();
    char title[48];
    snprintf(title, sizeof(title), LV_SYMBOL_SAVE "  %s", ui.disks->title);
    make_title(s, title);
    ui.disk_status = lv_label_create(s);
    lv_obj_set_width(ui.disk_status, lv_pct(100));
    lv_label_set_long_mode(ui.disk_status, LV_LABEL_LONG_DOT);
    /* one line: the drive cards start right below it */
    lv_obj_set_height(ui.disk_status, lv_font_get_line_height(lv_obj_get_style_text_font(ui.disk_status, 0)));
    lv_obj_align(ui.disk_status, LV_ALIGN_TOP_LEFT, 0, SY(44));

    static const char *const texts[3] = {
        LV_SYMBOL_DOWNLOAD "  Insert disk", LV_SYMBOL_PLUS "  New empty disk", LV_SYMBOL_EJECT "  Eject",
    };
    static const lv_event_cb_t cbs[3] = { insert_clicked, new_clicked, eject_clicked };
    static const int widths[3] = { 260, 300, 200 };
    /* Four drives (the Spectrum's): the name and shorter buttons in one row */
    static const char *const short_texts[3] = {
        LV_SYMBOL_DOWNLOAD "  Insert", LV_SYMBOL_PLUS "  New", LV_SYMBOL_EJECT "  Eject",
    };
    static const int short_widths[3] = { 170, 140, 150 };
    /* Cards share the room between the status line and the Back button
     * (sizes for 600 lines)
     */
    int n = ui.disks->drives;
    int gap = 12;
    int card_h = (356 - (n - 1) * gap) / n;
    if (card_h > 160) {
        card_h = 160;
    }
    bool compact = card_h < 100;
    int button_h = card_h >= 150 ? 72 : 56;
    for (int d = 0; d < n; d++) {
        lv_obj_t *card = lv_obj_create(s);
        lv_obj_set_size(card, lv_pct(100), SY(card_h));
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, SY(86 + d * (card_h + gap)));
        lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, SY(card_h >= 150 ? 16 : 10), 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        ui.drive_label[d] = lv_label_create(card);
        lv_label_set_long_mode(ui.drive_label[d], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ui.drive_label[d], &FONT_BIG, 0);
        if (compact) {
            int buttons = short_widths[0] + short_widths[1] + short_widths[2] + 2 * 12;
            lv_obj_set_width(ui.drive_label[d], BOARD_LCD_H_RES - 2 * PAD - 2 * SY(10) - SX(buttons + 16));
            lv_obj_align(ui.drive_label[d], LV_ALIGN_LEFT_MID, 0, 0);
        } else {
            lv_obj_set_width(ui.drive_label[d], lv_pct(100));
            lv_obj_align(ui.drive_label[d], LV_ALIGN_TOP_LEFT, 0, 0);
        }

        int x = 0;
        for (int b = compact ? 2 : 0; compact ? b >= 0 : b < 3; b += compact ? -1 : 1) {
            const char *text = compact ? short_texts[b] : texts[b];
            int w = compact ? short_widths[b] : widths[b];
            lv_obj_t *btn = make_button_for(card, text, cbs[b], w, (void *)(intptr_t)d);
            lv_obj_set_height(btn, SY(button_h));
            if (compact) {
                lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -SX(x), 0);   /* from the right: Eject first */
                x += w + 12;
            } else {
                lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, SX(x), 0);
                x += w + 16;
            }
            ui.drive_buttons[d][b] = btn;
        }
    }
    lv_obj_t *back = make_button(s, LV_SYMBOL_LEFT "  Back", back_to_status, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
#if CONFIG_EMU_WEB
    lv_obj_t *web = lv_label_create(s);
    lv_label_set_text(web, "Add disks from a computer:\nhttp://" CONFIG_EMU_HOSTNAME ".local");
    lv_obj_set_style_text_color(web, COLOR_DIM, 0);
    lv_obj_set_style_text_align(web, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(web, LV_ALIGN_BOTTOM_RIGHT, 0, -SY(6));
#endif

    s = ui.images_screen = make_screen();
    ui.images_title = make_title(s, "");
    ui.images_count = lv_label_create(s);
    lv_obj_set_style_text_color(ui.images_count, COLOR_DIM, 0);
    lv_obj_align(ui.images_count, LV_ALIGN_TOP_RIGHT, 0, SY(6));
    ui.images_list = make_list(s, 56, 380);
    back = make_button(s, LV_SYMBOL_LEFT "  Back", show_disks, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
}

/* ---- main loop -------------------------------------------------------------- */

void emu_ui_run(uint16_t *fb, bool running, emu_ui_result_t *result)
{
    memset(&ui, 0, sizeof(ui));
    ui.m = emu_machine();
    ui.disks = ui.m->disks;
    ui.fb = fb;
    ui.running = running;
    for (int i = 0; i < ui.m->choice_count && i < EMU_UI_MAX_CHOICES; i++) {
        ui.before[i] = ui.m->choices[i].get();
    }

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    size_t buf_size = BOARD_LCD_H_RES * BUF_LINES * sizeof(uint16_t);
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    build_status_screen();
    build_list_screen();
    build_pass_screen();
#if CONFIG_EMU_BLE_HID
    build_bt_screens();
#endif
    if (ui.disks != NULL) {
        build_disk_screens();
    }
    lv_screen_load(ui.status_screen);

    int64_t next_update = 0;
    while (!ui.done) {
        lv_timer_handler();
        if (esp_timer_get_time() >= next_update) {
            update_status();
            update_networks();
#if CONFIG_EMU_BLE_HID
            update_bluetooth();
#endif
            next_update = esp_timer_get_time() + 250000;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#if CONFIG_EMU_BLE_HID
    ble_hid_set_pairing(false);
#endif

    disks_free_list(ui.images, ui.image_count);
    result->reset = ui.reset;
    result->changed = choices_changed();
    memcpy(result->before, ui.before, sizeof(result->before));

    /* Hand the panel back: clear it and shut LVGL down */
    lv_deinit();
    free(buf);
    memset(fb, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
    lcd_flush(0, BOARD_LCD_V_RES);
}
