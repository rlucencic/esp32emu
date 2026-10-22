/*
 * Line console for tools/emu_console.py on the native USB port
 * (USB-Serial-JTAG), or on the USB-to-UART port when the speaker has taken
 * the native USB pins (MaTouch). Commands: see emu/console.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gptimer.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emu/board.h"
#include "emu/console.h"
#include "emu/disks.h"
#include "emu/speaker.h"
#include "emu/touch.h"
#include "emu_internal.h"

#define UART_BAUD       921600

static const char *TAG = "console";

#if CONFIG_EMU_SPEAKER && BOARD_SPEAKER_TAKES_USB

/* UART0 (the board's USB-to-UART bridge) */
static void console_write(const char *data, size_t len)
{
    uart_write_bytes(UART_NUM_0, data, len);
}

static bool console_read_line(char *line, size_t size)
{
    size_t n = 0;
    for (;;) {
        char c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || n + 1 >= size) {
            line[n] = 0;
            return true;
        }
        if (c != '\r') {
            line[n++] = c;
        }
    }
}

/* ESP logs go to the UART too: the native USB console is gone */
static int log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    console_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    return n;
}

static bool console_open(void)
{
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM_0, 1024, 16384, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_NUM_0, &cfg) != ESP_OK) {
        return false;
    }
    esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "console on UART0, %d baud", UART_BAUD);
    return true;
}

#else

/* USB-Serial-JTAG (native USB port) */
static void console_write(const char *data, size_t len)
{
    /* Straight to the driver in chunks that fit its TX ring; stdio would
     * drop data whenever the host is slower than 50 ms. Give up if the
     * host stops reading.
     */
    for (size_t off = 0; off < len;) {
        size_t chunk = len - off < 1024 ? len - off : 1024;
        int n = usb_serial_jtag_write_bytes(data + off, chunk, pdMS_TO_TICKS(2000));
        if (n <= 0) {
            break;
        }
        off += n;
    }
}

static bool console_read_line(char *line, size_t size)
{
    return fgets(line, size, stdin) != NULL;
}

static bool console_open(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        return false;
    }
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
    return true;
}

#endif

void emu_console_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    console_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

static void base64_line(const uint8_t *in, size_t len, char *out)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < len) v |= in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = i + 1 < len ? table[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? table[v & 63] : '=';
    }
    out[o] = 0;
}

static uint16_t *s_lcd_fb;

/* Screen as big-endian RGB565: the machine's screen at its own resolution,
 * or with `lcd` the whole panel. A few rows at a time (a whole screen and
 * its base64 would take MBs): 12 rows of any width are whole 96-byte lines.
 */
#define DUMP_ROWS   12

static void dump_screen(bool lcd)
{
    const emu_machine_t *m = emu_machine();
    emu_screen_t mode = { 0 };
    int repeat = 1;
    if (lcd) {
        if (s_lcd_fb == NULL) {
            return;
        }
        mode = (emu_screen_t){ .width = BOARD_LCD_H_RES, .height = BOARD_LCD_V_RES };
    } else {
        m->screen_mode(&mode);
        /* Pixel shape on the panel: view width / width wide, panel height / height tall */
        int view_w = mode.view_w > 0 ? mode.view_w : emu_view_width();
        int view_h = mode.view_h > 0 ? mode.view_h : BOARD_LCD_V_RES;
        int cols = mode.src_w > 0 ? mode.src_w : mode.width;
        repeat = (view_h * cols + view_w * mode.height / 2) / (view_w * mode.height);
        repeat = repeat < 1 ? 1 : repeat;
    }
    size_t row_bytes = mode.width * sizeof(uint16_t);
    size_t chunk = row_bytes * DUMP_ROWS;
    uint16_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    char *text = heap_caps_malloc(chunk / 96 * 130 + 160, MALLOC_CAP_SPIRAM);
    if (buf == NULL || text == NULL) {
        emu_console_printf("EMUSCREEN ERROR\n");
        free(buf);
        free(text);
        return;
    }

    /* stdout stays locked so log lines from other tasks wait meanwhile */
    fflush(stdout);
    flockfile(stdout);
    size_t len = sprintf(text, "\nEMUSCREEN %d %d %d\n", mode.width, mode.height, repeat);
    console_write(text, len);
    for (int y = 0; y < mode.height; y += DUMP_ROWS) {
        int rows = mode.height - y < DUMP_ROWS ? mode.height - y : DUMP_ROWS;
        for (int r = 0; r < rows; r++) {
            uint16_t *row = buf + r * mode.width;
            if (lcd) {
                memcpy(row, s_lcd_fb + (y + r) * BOARD_LCD_H_RES, row_bytes);
            } else {
                m->render_line(y + r, row);
            }
            for (int x = 0; x < mode.width; x++) {
                row[x] = __builtin_bswap16(row[x]);
            }
        }
        size_t bytes = rows * row_bytes;
        len = 0;
        for (size_t off = 0; off < bytes; off += 96) {
            size_t n = bytes - off < 96 ? bytes - off : 96;
            base64_line((const uint8_t *)buf + off, n, text + len);
            len += strlen(text + len);
            text[len++] = '\n';
        }
        console_write(text, len);
    }
    console_write("EMUSCREEN END\n", 14);
    funlockfile(stdout);
    free(buf);
    free(text);
}

/* Machine state for debugging */
static void print_info(void)
{
    const emu_machine_t *m = emu_machine();
    emu_info_t info;
    emu_info(&info);
    emu_screen_t mode = { 0 };
    char line[160] = "";
    if (emu_started()) {
        m->screen_mode(&mode);
        if (m->console_info) {
            m->console_info(line, sizeof(line));
        }
    }
    static const char *const states[] = { "setup", "running", "paused", "halted", "stopped" };
    emu_console_printf("EMUINFO %s %s mode %dx%d %d Hz signature %08lx %s\n", m->short_name,
                       states[info.state], mode.width, mode.height, mode.hz,
                       emu_started() ? (unsigned long)m->screen_signature() : 0ul, line);
}

typedef struct {
    int drive;
    const char *name;
} disk_cmd_t;

/* Insert `name`, "" ejects, "+" makes a new blank disk */
static void disk_command(void *arg)
{
    const disk_cmd_t *c = arg;
    char made[32];
    if (c->name[0] == 0) {
        disks_eject(c->drive);
    } else if (strcmp(c->name, "+") == 0) {
        disks_create(c->drive, disks_default_size(c->drive), made, sizeof(made));
    } else {
        disks_insert(c->drive, c->name);
    }
}

/* ---- p: a profile of a core (1: the emulation, 0: the presenter, WiFi, ...) ---- */

#define PROFILE_HZ      20000
#define PROFILE_MAX     32000

static uint32_t *s_prof;
static volatile int s_prof_n, s_prof_want;

/* The interrupted program counter (Xtensa: a level 1 interrupt's EPC1;
 * RISC-V: mepc)
 */
static bool IRAM_ATTR profile_tick(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *ctx)
{
    uint32_t pc;
#if __XTENSA__
    __asm__ volatile("rsr.epc1 %0" : "=a"(pc));
#else
    __asm__ volatile("csrr %0, mepc" : "=r"(pc));
#endif
    if (s_prof_n < s_prof_want) {
        s_prof[s_prof_n++] = pc;
    }
    return false;
}

/* On the core sampled, above the emulation (it doesn't yield while it's
 * behind), so the timer's interrupt is that core's
 */
static void profile_task(void *arg)
{
    TaskHandle_t console = arg;
    gptimer_handle_t t = NULL;
    gptimer_config_t cfg = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 1000000 };
    gptimer_event_callbacks_t cbs = { .on_alarm = profile_tick };
    gptimer_alarm_config_t alarm = { .alarm_count = 1000000 / PROFILE_HZ, .flags.auto_reload_on_alarm = true };
    if (gptimer_new_timer(&cfg, &t) == ESP_OK && gptimer_register_event_callbacks(t, &cbs, NULL) == ESP_OK &&
        gptimer_set_alarm_action(t, &alarm) == ESP_OK && gptimer_enable(t) == ESP_OK && gptimer_start(t) == ESP_OK) {
        while (s_prof_n < s_prof_want) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        gptimer_stop(t);
        gptimer_disable(t);
    }
    if (t) {
        gptimer_del_timer(t);
    }
    xTaskNotifyGive(console);
    vTaskDelete(NULL);
}

static void profile(int samples, int core)
{
    if (samples <= 0 || samples > PROFILE_MAX) {
        samples = 8000;
    }
    if (s_prof == NULL) {
        s_prof = heap_caps_malloc(PROFILE_MAX * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        if (s_prof == NULL) {
            emu_console_printf("EMUPROF END\n");
            return;
        }
    }
    s_prof_n = 0;
    s_prof_want = samples;
    xTaskCreatePinnedToCore(profile_task, "profile", 3072, xTaskGetCurrentTaskHandle(), 20, NULL, core == 0 ? 0 : 1);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(samples * 1000 / PROFILE_HZ + 5000));
    for (int i = 0; i < s_prof_n; i += 16) {
        char line[16 * 9 + 16];
        int n = snprintf(line, sizeof(line), "EMUPROF");
        for (int j = i; j < i + 16 && j < s_prof_n; j++) {
            n += snprintf(line + n, sizeof(line) - n, " %08lx", (unsigned long)s_prof[j]);
        }
        emu_console_printf("%s\n", line);
    }
    emu_console_printf("EMUPROF END\n");
}

static void console_task(void *arg)
{
    char line[128];
    for (;;) {
        if (!console_read_line(line, sizeof(line))) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        switch (line[0]) {
        case 's':
            if (emu_started()) {
                dump_screen(false);
            }
            break;
        case 'L':
            dump_screen(true);
            break;
        case 'r':
            emu_request_reset();
            break;
        case 'x': {
            /* Hold off this core's (core 0's) interrupts, as a flash write
             * does: the RGB panel loses refills and must be back in step
             * at the next VSYNC (lcd_resyncs())
             */
            int ms = atoi(line + 1);
            ms = ms < 1 ? 1 : ms > 100 ? 100 : ms;
            portDISABLE_INTERRUPTS();
            esp_rom_delay_us(ms * 1000);
            portENABLE_INTERRUPTS();
            break;
        }
        case 'F': {
            int drive = -1, at = 0;
            if (sscanf(line + 1, " %d %n", &drive, &at) >= 1 && drive >= 0 &&
                drive < disks_config()->drives) {
                disk_cmd_t c = { drive, line + 1 + at };
                emu_paused(disk_command, &c);
            }
            break;
        }
        case 't': {
            int x = 0, y = 0, ms = 150, x2 = 0, y2 = 0;
            int n = sscanf(line + 1, "%d %d %d %d %d", &x, &y, &ms, &x2, &y2);
            if (n == 5) {
                touch_inject_drag(x, y, x2, y2, ms);
            } else if (n >= 2) {
                touch_inject(x, y, ms);
            }
            break;
        }
        case 'i':
            print_info();
            break;
        case 'p': {
            int samples = 0, core = 1;
            sscanf(line + 1, "%d %d", &samples, &core);
            profile(samples, core);
            break;
        }
#if CONFIG_EMU_SPEAKER
        case 'v': {
            const char *arg = line + 1 + strspn(line + 1, " ");
            if (*arg == 'm') {
                speaker_set_muted(!speaker_muted());
            } else if (*arg != 0) {
                speaker_set_volume(atoi(arg));
            }
            if (*arg != 0) {
                speaker_save();
            }
            emu_console_printf("volume %d%s\n", speaker_volume(), speaker_muted() ? ", muted" : "");
            break;
        }
#endif
        default:
            if (emu_machine()->console_command) {
                emu_machine()->console_command(line);
            }
            break;
        }
    }
}

void emu_console_start(uint16_t *lcd_fb)
{
    s_lcd_fb = lcd_fb;
    if (!console_open()) {
        ESP_LOGE(TAG, "console driver install failed");
        return;
    }
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 3, NULL, 0);
}
