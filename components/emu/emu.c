/*
 * The emulator runtime: tasks, pacing, the presenter, pausing for the
 * setup screen, and the boot sequence (see emu.h).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "emu/ble_hid.h"
#include "emu/board.h"
#include "emu/disks.h"
#include "emu/lcd.h"
#include "emu/net.h"
#include "emu/sdcard.h"
#include "emu/speaker.h"
#include "emu/touch.h"
#include "emu_internal.h"
#include "emudbg.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "tab5.h"
#endif

#define VIEW_H          BOARD_LCD_V_RES
#define PRESENT_MS      40

/* Long-press here (top-right corner) for the setup screen: 100 pixels on
 * the 1024 x 600 MaTouch, as much of the screen elsewhere
 */
#define SETUP_ZONE_X    (BOARD_LCD_H_RES - BOARD_LCD_H_RES * 100 / 1024)
#define SETUP_ZONE_Y    (BOARD_LCD_V_RES * 100 / 600)
#define SETUP_HOLD_MS   2000
#define TOUCH_POLL_MS   20

static const char *TAG = "emu";

static const emu_machine_t *M;
static volatile bool s_reset_requested;
static volatile uint32_t s_frames;      /* written by the emulation task only */

/* Pausing the emulation (setup screen, disk changes) */
static volatile bool s_pause_request;
static volatile bool s_paused;
static volatile bool s_present_on = true;
static bool s_started;                  /* the emulation task runs */
static SemaphoreHandle_t s_pause_lock;

/* Set before restarting the board for a big memory block: start the machine
 * right away afterwards instead of showing the setup screen
 */
#define START_AFTER_RESTART     0x454D5521u
static RTC_NOINIT_ATTR uint32_t s_start_after_restart;

static emu_info_t s_info = { .temperature = NAN };
static int64_t s_session_start;
static uint16_t *s_fb;
static char s_drive_info[96];
static char s_message[128];

const emu_machine_t *emu_machine(void)
{
    return M;
}

const char *emu_drive_info(void)
{
    return s_drive_info;
}

void emu_set_drive_info(const char *text)
{
    snprintf(s_drive_info, sizeof(s_drive_info), "%s", text);
}

const char *emu_message(void)
{
    return s_message;
}

void emu_set_message(const char *text)
{
    snprintf(s_message, sizeof(s_message), "%s", text ? text : "");
}

bool emu_started(void)
{
    return s_started;
}

/* view_width is for 600 lines: as wide in proportion to this panel's height */
int emu_view_width(void)
{
    int w = (M->view_width * VIEW_H / 600 + 1) & ~1;
    return w > BOARD_LCD_H_RES ? BOARD_LCD_H_RES : w;
}

void emu_request_reset(void)
{
    s_reset_requested = true;
}

void emu_reboot_into_machine(void)
{
    s_start_after_restart = START_AFTER_RESTART;
    esp_restart();
}

/* ---- The emulation task (core 1) ---- */

static void emulation_task(void *arg)
{
    int64_t next = esp_timer_get_time();

    for (;;) {
        if (s_pause_request) {
            s_paused = true;
            while (s_pause_request) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            s_paused = false;
            next = esp_timer_get_time();
        }
        if (s_reset_requested) {
            s_reset_requested = false;
            M->reset();
            emudbg_rearm();
        }
        M->run_frame();
        s_frames++;
        emudbg_frame_end();

#if CONFIG_EMU_SPEAKER
        /* The speaker's I2S clock sets the pace: stay ~2 frames ahead of it */
        if (M->audio_pending != NULL) {
            while (M->audio_pending() > M->audio_hz / 25 && !s_pause_request) {
                vTaskDelay(1);
            }
            continue;
        }
#endif
        emu_screen_t mode;
        M->screen_mode(&mode);
        next += 1000000 / mode.hz;
        int64_t now = esp_timer_get_time();
        if (next > now) {
            vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        } else if (now - next > 100000) {
            next = now;     /* too far behind: don't try to catch up */
        }
    }
}

/* Stop the emulation task between frames (and the presenter) */
static void pause(void)
{
    s_present_on = false;
    s_pause_request = true;
    emudbg_suspend();           /* out of a debugger stop: on to the frame's end */
    while (!s_paused) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void resume(void)
{
    s_present_on = true;
    s_pause_request = false;
    emudbg_resume();
}

bool emu_paused(void (*fn)(void *arg), void *arg)
{
    /* The setup screen holds the lock while it's open */
    if (xSemaphoreTake(s_pause_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return false;
    }
    if (s_started) {
        pause();
    }
    fn(arg);
    if (s_started) {
        resume();
    }
    xSemaphoreGive(s_pause_lock);
    return true;
}

static void start_fn(void *arg)
{
    bool *ok = arg;
    *ok = M->start();
    if (*ok) {
        s_session_start = esp_timer_get_time();
    }
}

bool emu_restart(void)
{
    bool ok = false;
    return emu_paused(start_fn, &ok) && ok;
}

void emu_info(emu_info_t *info)
{
    *info = s_info;
    info->state = !s_started ? EMU_SETUP : s_paused ? EMU_PAUSED : EMU_RUNNING;
    if (info->state == EMU_RUNNING && M->halted && M->halted()) {
        info->state = EMU_HALTED;
    }
    if (info->state == EMU_RUNNING && emudbg_stopped()) {
        info->state = EMU_STOPPED;
    }
    info->session_s = s_started ? (esp_timer_get_time() - s_session_start) / 1000000 : 0;
}

/* ---- Chip temperature, on request ----
 *
 * Reading the sensor blocks interrupts on the reading core for a moment,
 * which on core 0 made the LCD's bounce buffer refill late (picture
 * artefacts). So it's read on core 1, above the emulation's priority, with
 * the sensor powered only while it's read.
 */
#define TEMPERATURE_EVERY_US    5000000

static TaskHandle_t s_temperature_task;

static void temperature_task(void *arg)
{
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &tsens) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        float celsius;
        if (temperature_sensor_enable(tsens) == ESP_OK) {
            if (temperature_sensor_get_celsius(tsens, &celsius) == ESP_OK) {
                s_info.temperature = celsius;
            }
            temperature_sensor_disable(tsens);
        }
    }
}

void emu_measure_temperature(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (s_temperature_task != NULL && (last == 0 || now - last >= TEMPERATURE_EVERY_US)) {
        last = now;
        xTaskNotifyGive(s_temperature_task);
    }
}

/* ---- Every second (core 0): speed, core loads, the machine's chores ---- */

static void stats_task(void *arg)
{
    uint32_t last_frames = s_frames;
    int64_t last = esp_timer_get_time();
    uint32_t last_idle[2] = { ulTaskGetIdleRunTimeCounterForCore(0),
                              ulTaskGetIdleRunTimeCounterForCore(1) };
    uint32_t last_resyncs = 0;
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        disks_poll();
        if (M->every_second) {
            M->every_second();
        }

        uint32_t resyncs = lcd_resyncs();
        if (resyncs != last_resyncs) {
            ESP_LOGW(TAG, "LCD output was out of step, put right at VSYNC (%lu times since start)",
                     (unsigned long)resyncs);
            last_resyncs = resyncs;
        }

        uint32_t frames = s_frames;
        int64_t now = esp_timer_get_time();
        float elapsed = now - last;
        for (int core = 0; core < 2; core++) {
            uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
            float busy = 100.0f - (idle - last_idle[core]) * 100.0f / elapsed;
            s_info.load[core] = busy < 0 ? 0 : busy > 100 ? 100 : busy;
            last_idle[core] = idle;
        }

        if (s_started && !s_paused) {
            emu_screen_t mode;
            M->screen_mode(&mode);
            s_info.fps = (frames - last_frames) * 1e6f / elapsed;
            s_info.speed = (int)(s_info.fps * 100 / mode.hz + 0.5f);
            if (tick % 5 == 0) {
                char line[160] = "";
                if (M->console_info) {
                    M->console_info(line, sizeof(line));
                }
                ESP_LOGI(TAG, "%.1f fps (%d%% of %d Hz) %s", s_info.fps, s_info.speed, mode.hz, line);
            }
        } else {
            s_info.fps = 0;
            s_info.speed = 0;
        }
        last_frames = frames;
        last = now;
    }
}

/* ---- Presenter (core 0): the machine's screen onto the panel ---- */

static inline uint32_t expand565(uint16_t c)
{
    return (c | ((uint32_t)c << 16)) & 0x07E0F81F;
}

static inline uint16_t blend565(uint16_t a, uint16_t b, int w)
{
    if (a == b || w == 0) {
        return a;
    }
    uint32_t c = (expand565(a) * (32 - w) + expand565(b) * w) >> 5;
    c &= 0x07E0F81F;
    return (uint16_t)(c | (c >> 16));
}

typedef struct {
    uint16_t pos;               /* source pixel */
    uint8_t w;                  /* weight of the next one, 0..32 */
} tap_t;

/* Source position of each destination pixel, sampled at pixel centres */
static void make_taps(tap_t *taps, int dst, int src, bool smooth)
{
    for (int i = 0; i < dst; i++) {
        if (!smooth) {
            taps[i].pos = i * src / dst;
            taps[i].w = 0;
            continue;
        }
        int32_t p = ((2 * i + 1) * src * 32) / (2 * dst) - 16;    /* 1/32 pixel */
        if (p < 0) {
            p = 0;
        }
        int pos = p >> 5;
        int w = p & 31;
        if (pos >= src - 1) {
            pos = src - 1;
            w = 0;
        }
        taps[i].pos = pos;
        taps[i].w = w;
    }
}

static void scale_row(const uint16_t *src, uint16_t *dst, const tap_t *taps, int n)
{
    for (int x = 0; x < n; x++) {
        const tap_t t = taps[x];
        dst[x] = t.w ? blend565(src[t.pos], src[t.pos + 1], t.w) : src[t.pos];
    }
}

static uint32_t line_hash(const uint16_t *line, int width)
{
    const uint32_t *p = (const uint32_t *)line;
    uint32_t h = 2166136261u;
    for (int i = 0; i < width / 2; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Only rows whose source lines changed are scaled and written: every write
 * to the PSRAM framebuffer competes with the LCD's own reads of it, and too
 * many make the refill late (artefacts, a picture shifted by some lines)
 */
#define MAX_SOURCE_LINES    1024

static void present_task(void *arg)
{
    uint16_t *fb = arg;
    const int view_w = emu_view_width();
    const int view_x = (BOARD_LCD_H_RES - view_w) / 2;
    const bool smooth = M->scale == EMU_SCALE_SMOOTH;
    uint16_t *src = heap_caps_malloc(1024 * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    uint16_t *rows[2] = {
        heap_caps_malloc(view_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL),
        heap_caps_malloc(view_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL),
    };
    uint16_t *out = heap_caps_malloc(view_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    tap_t *xtaps = heap_caps_malloc(view_w * sizeof(tap_t), MALLOC_CAP_INTERNAL);
    static tap_t ytaps[VIEW_H];
    uint32_t *hashes = heap_caps_calloc(MAX_SOURCE_LINES, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    uint8_t *changed = heap_caps_malloc(MAX_SOURCE_LINES, MALLOC_CAP_SPIRAM);
    uint32_t last_signature = 0;
    int last_width = 0, last_height = 0;
    bool redraw = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PRESENT_MS));
        if (!s_present_on) {
            last_signature = 0;     /* redraw everything afterwards */
            redraw = true;
            continue;
        }
        uint32_t signature = M->screen_signature();
        if (signature == last_signature) {
            continue;
        }
        last_signature = signature;

        emu_screen_t mode;
        M->screen_mode(&mode);
        if (mode.width > 1024 || mode.height > MAX_SOURCE_LINES) {
            continue;
        }
        if (mode.width != last_width || mode.height != last_height) {
            make_taps(xtaps, view_w, mode.width, smooth);
            make_taps(ytaps, VIEW_H, mode.height, smooth);
            last_width = mode.width;
            last_height = mode.height;
            redraw = true;
        }

        /* Which source lines changed since the last time */
        for (int y = 0; y < mode.height; y++) {
            M->render_line(y, src);
            uint32_t h = line_hash(src, mode.width);
            changed[y] = redraw || h != hashes[y];
            hashes[y] = h;
        }
        redraw = false;

        /* Scaled source rows, kept while consecutive output rows use them */
        int have[2] = { -1, -1 };
        int first = VIEW_H, last = -1;      /* rows written */
        for (int r = 0; r < VIEW_H; r++) {
            int y = ytaps[r].pos, w = ytaps[r].w;
            if (!changed[y] && (!w || !changed[y + 1])) {
                continue;
            }
            int need[2] = { y, w ? y + 1 : -1 };
            for (int k = 0; k < 2; k++) {
                if (need[k] < 0 || have[k] == need[k]) {
                    continue;
                }
                if (k == 0 && have[1] == need[0]) {
                    /* Moving down one row: the second becomes the first */
                    uint16_t *t = rows[0];
                    rows[0] = rows[1];
                    rows[1] = t;
                    have[0] = have[1];
                    have[1] = -1;
                    continue;
                }
                M->render_line(need[k], src);
                scale_row(src, rows[k], xtaps, view_w);
                have[k] = need[k];
            }
            const uint16_t *line = rows[0];
            if (w) {
                for (int x = 0; x < view_w; x++) {
                    out[x] = blend565(rows[0][x], rows[1][x], w);
                }
                line = out;
            }
            memcpy(fb + r * BOARD_LCD_H_RES + view_x, line, view_w * sizeof(uint16_t));
            first = r < first ? r : first;
            last = r;
        }
        lcd_flush(first, last + 1);
    }
}

/* ---- Setup screen and touch (core 0) ---- */

#if CONFIG_EMU_SETUP_SCREEN

/* Pause the machine, show the setup screen, restart it if a choice changed */
static void setup_while_running(void)
{
    xSemaphoreTake(s_pause_lock, portMAX_DELAY);
    pause();
    emu_ui_result_t r;
    emu_ui_run(s_fb, true, &r);
    if (r.changed) {
        ESP_LOGI(TAG, "restarting the %s with the new settings", M->short_name);
        if (M->start()) {
            s_session_start = esp_timer_get_time();
        } else {
            /* e.g. that ROM was never flashed: back to the old settings */
            for (int i = 0; i < M->choice_count && i < EMU_UI_MAX_CHOICES; i++) {
                M->choices[i].set(r.before[i]);
            }
            M->start();
        }
    } else if (r.reset) {
        M->reset();
    }
    resume();
    xSemaphoreGive(s_pause_lock);
}

#endif

/* Touches: a long press in the top-right corner opens the setup screen;
 * on the picture they go to the machine if it wants them
 */
static void supervisor_task(void *arg)
{
    int64_t held_since = 0;
    bool forwarding = false;
    int last_x = 0, last_y = 0;
    const int view_w = emu_view_width();
    const int view_x = (BOARD_LCD_H_RES - view_w) / 2;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        int x = 0, y = 0;
        bool pressed = false;
        touch_read(&x, &y, &pressed);
        bool in_zone = x >= SETUP_ZONE_X && y <= SETUP_ZONE_Y;

        if (M->touch != NULL && !s_paused) {
            emu_screen_t mode;
            M->screen_mode(&mode);
            if (pressed && (forwarding || (!in_zone && x >= view_x && x < view_x + view_w))) {
                int mx = (x - view_x) * mode.width / view_w;
                int my = y * mode.height / VIEW_H;
                last_x = mx < 0 ? 0 : mx >= mode.width ? mode.width - 1 : mx;
                last_y = my;
                M->touch(last_x, last_y, true);
                forwarding = true;
            } else if (!pressed && forwarding) {
                M->touch(last_x, last_y, false);
                forwarding = false;
            }
        }

#if CONFIG_EMU_SETUP_SCREEN
        if (!pressed || !in_zone || forwarding) {
            held_since = 0;
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (held_since == 0) {
            held_since = now;
        } else if (now - held_since >= SETUP_HOLD_MS * 1000LL) {
            held_since = 0;
            /* Wait for the finger to lift so the setup screen doesn't see it */
            do {
                vTaskDelay(pdMS_TO_TICKS(50));
                touch_read(&x, &y, &pressed);
            } while (pressed);
            setup_while_running();
        }
#endif
    }
}

/* ---- Boot ---- */

static void hid_key(uint8_t usage, bool pressed)
{
    if (M->hid_key) {
        M->hid_key(usage, pressed);
    }
}

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    if (M->hid_mouse) {
        M->hid_mouse(dx, dy, buttons);
    }
}

void emu_run(const emu_machine_t *machine)
{
    M = machine;
    s_pause_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    bool start_now = s_start_after_restart == START_AFTER_RESTART;
    s_start_after_restart = 0;
    if (M->reserve_memory) {
        M->reserve_memory();
    }
#if CONFIG_IDF_TARGET_ESP32P4
    tab5_init();            /* the C6 boots meanwhile */
#endif

    bool sd = sdcard_mount();
    s_info.sd_card = sd;
    if (M->storage) {
        M->storage(sd);
    }
    if (s_drive_info[0] == 0) {
        emu_set_drive_info(sd ? "Disk images on the SD card" : "No SD card: no disk images");
    }
    if (sd && M->disks) {
        disks_init(M->disks);
        disks_restore();        /* before the machine starts: a disk in the first drive boots */
    }

    s_fb = lcd_init();
    if (s_fb != NULL) {
        memset(s_fb, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
        lcd_flush(0, BOARD_LCD_V_RES);
    }
    touch_init();
    emu_console_start(s_fb);

#if CONFIG_IDF_TARGET_ESP32P4
    bool radio = tab5_radio_start();    /* WiFi and Bluetooth are the C6's */
#else
    bool radio = true;
#endif
#if CONFIG_EMU_WIFI
    bool net = radio && net_init();     /* before BLE: WiFi needs the larger internal RAM blocks */
#if CONFIG_EMU_WEB
    if (net) {
        emu_web_start();
    }
#endif
#if CONFIG_EMUDBG
    if (net) {
        emudbg_serve(CONFIG_EMUDBG_PORT);
    }
#endif
#endif
#if CONFIG_EMU_BLE_HID
#if CONFIG_IDF_TARGET_ESP32P4
    radio = radio && tab5_bt_start();
#endif
    if (err == ESP_OK && radio) {
        static const ble_hid_callbacks_t callbacks = { .key = hid_key, .mouse = hid_mouse };
        ble_hid_start(&callbacks);
    }
#endif
#if CONFIG_IDF_TARGET_ESP32P4
    tab5_keyboard_start(hid_key);
#endif
    (void)radio;
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(temperature_task, "temperature", 2560, NULL, 11,
                            &s_temperature_task, 1);

#if CONFIG_EMU_SPEAKER
    if (M->audio_enable) {
        M->audio_enable(true);
    }
#endif
    /* The setup screen first; again whenever the machine can't start */
    for (bool first = true;; first = false) {
#if CONFIG_EMU_SETUP_SCREEN
        if (s_fb != NULL && (!start_now || !first)) {
            emu_ui_result_t r;
            emu_ui_run(s_fb, false, &r);
        }
#endif
        if (M->start()) {
            emu_set_message(NULL);
            break;
        }
        if (s_message[0] == 0) {
            emu_set_message("The machine couldn't start: see the log.");
        }
#if !CONFIG_EMU_SETUP_SCREEN
        return;
#endif
        if (s_fb == NULL) {
            return;
        }
    }
    s_session_start = esp_timer_get_time();

    if (s_fb != NULL) {
        xTaskCreatePinnedToCore(present_task, "present", 4096, s_fb, 5, NULL, 0);
    }
#if CONFIG_EMU_SPEAKER
    if (M->audio_render) {
        speaker_start(M->audio_hz, M->audio_render, CONFIG_EMU_SPEAKER_VOLUME);
    }
#endif
    xTaskCreatePinnedToCore(emulation_task, "emulation", 8192, NULL, 10, NULL, 1);
    s_started = true;
    if (s_fb != NULL) {
        xTaskCreatePinnedToCore(supervisor_task, "supervisor", 16384, NULL, 4, NULL, 0);
    }

    ESP_LOGI(TAG, "Free internal: %u bytes, PSRAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
