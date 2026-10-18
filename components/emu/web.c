/*
 * Web page for the disk images and the machine: http://<CONFIG_EMU_HOSTNAME>.local
 *
 *   GET    /                    the page (web/index.html)
 *   GET    /machine.js          the machine's script for the page, if it has one
 *   GET    /api/info            machine name, drives, image kinds, blank disk sizes
 *   GET    /api/disks           {"ready", "free", "disks": [{"name", "size", "drive"}]}
 *   GET    /api/status          machine, drives, board and Bluetooth status
 *   PUT    /api/drive/<n>       insert the image named in the body into drive n
 *   DELETE /api/drive/<n>       eject drive n
 *   POST   /api/new?size=<bytes>  make a blank disk image
 *   PUT    /disks/<name>        upload (raw body; also POST), replaces an image
 *                               of that name unless it's in a drive
 *   GET    /disks/<name>        download
 *   DELETE /disks/<name>        delete, unless it's in a drive
 *
 * /floppies/<name> works as /disks/<name> (older scripts). Uploads go to a
 * temporary file first (<name>.tmp), so a broken transfer leaves no half
 * disk behind. Uploads and downloads run in tasks of their own (two at a
 * time), so the server keeps answering meanwhile: big disk images take
 * minutes. On the Tab5 a file goes through PSRAM whole (WHOLE_FILES).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "emu/ble_hid.h"
#include "emu/board.h"
#include "emu/disks.h"
#include "emu/net.h"
#include "emu/sdcard.h"
#include "emu_internal.h"

#define CHUNK           8192

/* The Tab5: its SD card and the C6's WiFi link share the SDMMC host, so a
 * transfer that alternates between them waits on both, and while the card
 * writes the C6 drops packets (32 KB pieces: a third of the speed). With
 * PSRAM to spare, a whole file is received before it's written and read
 * before it's sent; the SD driver takes PSRAM buffers aligned to the cache
 * line directly. Only with WHOLE_RESERVE left over, else in pieces.
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define WHOLE_FILES     1
#define WHOLE_ALIGN     128
#define WHOLE_RESERVE   (4 * 1024 * 1024)
#endif
#define STATUS_SIZE     4096
#define TRANSFERS       2           /* uploads + downloads at the same time */

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* "%20" etc. in place; false if malformed */
static bool url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '%') {
            unsigned v;
            if (!s[1] || !s[2] || sscanf(s + 1, "%2x", &v) != 1) {
                return false;
            }
            *out++ = v;
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = 0;
    return true;
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

static esp_err_t send_json(httpd_req_t *req, emu_json_t *j)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, j->buf, j->len);
}

/* Disk name from the URI after its second '/', checked; false sends the error */
static bool disk_name(httpd_req_t *req, char *name, size_t len)
{
    const char *p = strchr(req->uri + 1, '/');
    p = p ? p + 1 : "";
    snprintf(name, len, "%.*s", (int)strcspn(p, "?#"), p);
    if (!url_decode(name) || !disks_valid_name(name)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Not a disk image name: %s", disks_config()->description);
        send_text(req, "400 Bad Request", msg);
        return false;
    }
    return true;
}

static void disk_path(char *path, size_t len, const char *name)
{
    snprintf(path, len, "%s/%s", disks_folder(), name);
}

static esp_err_t send_conflict(httpd_req_t *req, const char *name)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%.100s is in a drive: eject it first", name);
    return send_text(req, "409 Conflict", msg);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

/* The machine's own script for the page (converters) */
static esp_err_t script_get(httpd_req_t *req)
{
    const emu_machine_t *m = emu_machine();
    if (!m->web_script) {
        return send_text(req, "404 Not Found", "No script");
    }
    httpd_resp_set_type(req, "text/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, m->web_script, m->web_script_end - m->web_script);
}

/* ---- What the machine is ---- */

static esp_err_t info_get(httpd_req_t *req)
{
    char buf[1024];
    emu_json_t j;
    emu_json_begin(&j, buf, sizeof(buf));
    const emu_machine_t *m = emu_machine();
    const disks_config_t *d = disks_config();
    emu_json_str(&j, "name", m->name);
    emu_json_str(&j, "short_name", m->short_name);
    emu_json_str(&j, "hostname", CONFIG_EMU_HOSTNAME);
    emu_json_str(&j, "board", BOARD_SHORT_NAME);
    emu_json_bool(&j, "script", m->web_script != NULL);
    emu_json_object(&j, "disks");
    emu_json_str(&j, "title", d->title);
    emu_json_str(&j, "description", d->description);
    emu_json_str(&j, "folder", d->folder);
    emu_json_int(&j, "max_upload", (long long)d->max_upload);
    emu_json_bool(&j, "can_create", d->create != NULL);
    emu_json_array(&j, "drives");
    for (int i = 0; i < d->drives; i++) {
        emu_json_str(&j, NULL, d->drive_names[i]);
    }
    emu_json_end_array(&j);
    emu_json_array(&j, "extensions");
    for (const char *const *e = d->extensions; e && *e; e++) {
        emu_json_str(&j, NULL, *e);
    }
    emu_json_end_array(&j);
    emu_json_array(&j, "sizes");
    for (int i = 0; i < d->size_count; i++) {
        emu_json_object(&j, NULL);
        emu_json_int(&j, "bytes", d->sizes[i].bytes);
        emu_json_str(&j, "label", d->sizes[i].label);
        emu_json_end_object(&j);
    }
    emu_json_end_array(&j);
    emu_json_end_object(&j);
    emu_json_end_object(&j);
    return send_json(req, &j);
}

static esp_err_t list_get(httpd_req_t *req)
{
    char **names;
    int n = disks_list(&names);
    uint64_t total = 0, free_bytes = 0;
    sdcard_space(&total, &free_bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ready\":%s,\"free\":%llu,\"disks\":[",
             disks_ready() ? "true" : "false", (unsigned long long)free_bytes);
    httpd_resp_sendstr_chunk(req, buf);
    for (int i = 0; i < n; i++) {
        char path[160], item[256];
        struct stat st;
        disk_path(path, sizeof(path), names[i]);
        long long size = stat(path, &st) == 0 ? (long long)st.st_size : 0;
        const char *drive = "";
        for (int d = 0; d < disks_config()->drives; d++) {
            if (disks_name(d) != NULL && strcmp(disks_name(d), names[i]) == 0) {
                drive = disks_config()->drive_names[d];
            }
        }
        emu_json_t j = { item, 0, sizeof(item), 0, { true } };
        emu_json_object(&j, NULL);
        emu_json_str(&j, "name", names[i]);
        emu_json_int(&j, "size", size);
        emu_json_str(&j, "drive", drive);
        emu_json_end_object(&j);
        if (i) {
            httpd_resp_sendstr_chunk(req, ",");
        }
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    disks_free_list(names, n);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- Status ---- */

static esp_err_t status_get(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(STATUS_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return send_text(req, "500 Internal Server Error", "Out of memory");
    }
    emu_json_t j;
    emu_json_begin(&j, buf, STATUS_SIZE);
    const emu_machine_t *m = emu_machine();
    emu_info_t info;
    emu_measure_temperature();
    emu_info(&info);
    static const char *const states[] = { "setup", "running", "paused", "halted", "stopped" };

    /* The machine */
    emu_json_object(&j, "machine");
    emu_json_str(&j, "name", m->name);
    emu_json_str(&j, "state", states[info.state]);
    if (emu_started()) {
        emu_screen_t mode;
        m->screen_mode(&mode);
        emu_json_int(&j, "hz", mode.hz);
        emu_json_num(&j, "fps", info.fps);
        emu_json_int(&j, "speed", info.speed);
        emu_json_int(&j, "session", info.session_s);
        emu_json_array(&j, "rows");
        if (m->status) {
            m->status(&j, EMU_STATUS_ROWS);
        }
        emu_json_end_array(&j);
    }
    emu_json_str(&j, "message", emu_message()[0] ? emu_message() : NULL);
    time_t now = time(NULL);
    struct tm tm;
    char clock[32];
    localtime_r(&now, &tm);
    strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &tm);
    emu_json_str(&j, "clock", clock);
    net_time_t from = net_time_source();
    emu_json_bool(&j, "clock_set", from != NET_TIME_NONE);
    emu_json_str(&j, "clock_from", from == NET_TIME_INTERNET ? "internet" :
                                   from == NET_TIME_BOARD ? "board" : NULL);
    emu_json_end_object(&j);

    /* Drives */
    emu_json_array(&j, "drives");
    if (m->status) {
        m->status(&j, EMU_STATUS_DRIVES);
    }
    emu_json_end_array(&j);

    /* The board */
    char ip[20] = "";
    wifi_ap_record_t ap;
    bool connected = net_state() == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (connected) {
        net_ip(ip, sizeof(ip));
    }
    uint64_t total = 0, free_bytes = 0;
    bool sd = sdcard_space(&total, &free_bytes);
    emu_json_object(&j, "board");
    emu_json_str(&j, "name", BOARD_NAME);
    emu_json_int(&j, "uptime", esp_timer_get_time() / 1000000);
    emu_json_object(&j, "wifi");
    emu_json_str(&j, "ssid", connected ? net_ssid() : NULL);
    emu_json_str(&j, "ip", connected ? ip : NULL);
    emu_json_int(&j, "rssi", connected ? ap.rssi : 0);
    emu_json_end_object(&j);
    emu_json_int(&j, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    emu_json_int(&j, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    emu_json_int(&j, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    emu_json_int(&j, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    emu_json_int(&j, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    emu_json_array(&j, "load");
    emu_json_int(&j, NULL, lroundf(info.load[0]));
    emu_json_int(&j, NULL, lroundf(info.load[1]));
    emu_json_end_array(&j);
    emu_json_num(&j, "temperature", info.temperature);
    emu_json_int(&j, "sd_total", sd ? (long long)total : 0);
    emu_json_int(&j, "sd_free", sd ? (long long)free_bytes : 0);
    emu_json_end_object(&j);

    /* Bluetooth */
    emu_json_array(&j, "bluetooth");
#if CONFIG_EMU_BLE_HID
    ble_hid_device_t dev[4];
    int n = ble_hid_devices(dev, 4);
    static const char *const kinds[] = { "device", "keyboard", "mouse" };
    for (int i = 0; i < n; i++) {
        emu_json_object(&j, NULL);
        emu_json_str(&j, "name", dev[i].name);
        emu_json_str(&j, "kind", kinds[dev[i].kind < 3 ? dev[i].kind : 0]);
        emu_json_bool(&j, "connected", dev[i].connected);
        if (dev[i].battery >= 0) {
            emu_json_int(&j, "battery", dev[i].battery);
        } else {
            emu_json_str(&j, "battery", NULL);
        }
        emu_json_end_object(&j);
    }
#endif
    emu_json_end_array(&j);
    emu_json_end_object(&j);

    esp_err_t err = send_json(req, &j);
    free(buf);
    return err;
}

/* ---- Drives ---- */

typedef struct {
    int drive;
    const char *name;           /* NULL: eject */
    bool ok;
} drive_op_t;

static void drive_op(void *arg)
{
    drive_op_t *op = arg;
    if (op->name == NULL) {
        disks_eject(op->drive);
        op->ok = true;
    } else {
        op->ok = disks_insert(op->drive, op->name);
    }
}

static int drive_number(httpd_req_t *req)
{
    int n = atoi(req->uri + strlen("/api/drive/"));
    return n >= 1 && n <= disks_config()->drives ? n - 1 : -1;
}

static esp_err_t drive_put(httpd_req_t *req)
{
    int d = drive_number(req);
    char name[DISKS_NAME_LEN];
    int len = req->content_len < sizeof(name) ? req->content_len : 0;
    if (d < 0 || len == 0 || httpd_req_recv(req, name, len) != len) {
        return send_text(req, "400 Bad Request", "PUT /api/drive/<number> with a disk image name");
    }
    name[len] = 0;
    if (!disks_valid_name(name)) {
        return send_text(req, "400 Bad Request", "Not a disk image name");
    }
    drive_op_t op = { d, name, false };
    if (!emu_paused(drive_op, &op)) {
        return send_text(req, "409 Conflict", "The setup screen is open on the board");
    }
    if (!op.ok) {
        return send_text(req, "422 Unprocessable Entity", "Not a disk image the machine can use");
    }
    ESP_LOGI(TAG, "%s into %s", name, disks_config()->drive_names[d]);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t drive_delete(httpd_req_t *req)
{
    int d = drive_number(req);
    if (d < 0) {
        return send_text(req, "400 Bad Request", "No such drive");
    }
    drive_op_t op = { d, NULL, false };
    if (!emu_paused(drive_op, &op)) {
        return send_text(req, "409 Conflict", "The setup screen is open on the board");
    }
    return send_text(req, "200 OK", "OK");
}

static esp_err_t new_post(httpd_req_t *req)
{
    char query[64], value[16];
    uint32_t bytes = disks_default_size(-1);
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "size", value, sizeof(value)) == ESP_OK) {
        bytes = strtoul(value, NULL, 10);
    }
    bool known = false;
    for (int i = 0; i < disks_config()->size_count; i++) {
        known |= disks_config()->sizes[i].bytes == bytes;
    }
    if (!known || !disks_ready()) {
        return send_text(req, "400 Bad Request", "No such disk size, or no SD card");
    }
    char name[32];
    if (!disks_create(-1, bytes, name, sizeof(name))) {
        return send_text(req, "500 Internal Server Error", "Couldn't make the disk image");
    }
    return send_text(req, "201 Created", name);
}

/* ---- Disk image files ---- */

/* Straight to the file system, past stdio's buffer: FATFS then reads and
 * writes runs of sectors in the caller's buffer
 */
static bool write_all(FILE *f, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fileno(f), p, n);
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= w;
    }
    return true;
}

static bool read_all(FILE *f, char *p, size_t n)
{
    while (n > 0) {
        ssize_t r = read(fileno(f), p, n);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= r;
    }
    return true;
}

/* A buffer for a whole file in PSRAM, or NULL: then in pieces */
static char *whole_alloc(size_t size)
{
#if WHOLE_FILES
    size_t rounded = (size + WHOLE_ALIGN - 1) / WHOLE_ALIGN * WHOLE_ALIGN;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= rounded + WHOLE_RESERVE) {
        return heap_caps_aligned_alloc(WHOLE_ALIGN, rounded, MALLOC_CAP_SPIRAM);
    }
#endif
    (void)size;
    return NULL;
}

/* The request's body into the file */
static bool receive_file(httpd_req_t *req, FILE *f)
{
    size_t size = req->content_len;
    char *whole = whole_alloc(size);
    if (whole != NULL) {
        size_t got = 0;
        while (got < size) {
            int n = httpd_req_recv(req, whole + got, size - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            got += n;
        }
        bool ok = got == size && write_all(f, whole, size);
        heap_caps_free(whole);
        return ok;
    }

    /* DMA-capable: the SD driver then writes whole runs of sectors at once
     * instead of copying them one by one (several times faster)
     */
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    if (buf == NULL) {
        return false;
    }
    size_t left = size;
    bool ok = true;
    while (left > 0 && ok) {
        int n = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ok = n > 0 && fwrite(buf, 1, n, f) == (size_t)n;
        left -= n > 0 ? n : 0;
    }
    free(buf);
    return ok;
}

static esp_err_t disk_put_work(httpd_req_t *req)
{
    char name[100], path[160], tmp[170];
    if (!disk_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    if (!disks_ready()) {
        return send_text(req, "500 Internal Server Error", "No SD card");
    }
    if (disks_in_use(name)) {
        return send_conflict(req, name);
    }
    if (req->content_len == 0 || req->content_len > disks_config()->max_upload) {
        return send_text(req, "400 Bad Request", "Empty, or too big for a disk image");
    }

    disk_path(path, sizeof(path), name);
    /* One per upload: two at a time shared "upload.tmp", and the second failed */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return send_text(req, "500 Internal Server Error", "Can't write to the SD card");
    }
    bool ok = receive_file(req, f);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        disks_removed(name);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        ESP_LOGW(TAG, "upload of %s failed", name);
        return send_text(req, "500 Internal Server Error", "Upload failed");
    }
    ESP_LOGI(TAG, "received %s, %u bytes", name, (unsigned)req->content_len);
    return send_text(req, "201 Created", "OK");
}

static esp_err_t disk_get_work(httpd_req_t *req)
{
    char name[100], path[160];
    if (!disk_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    disk_path(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (f == NULL || fstat(fileno(f), &st) != 0) {
        if (f) {
            fclose(f);
        }
        return send_text(req, "404 Not Found", "No such disk");
    }
    char disposition[140];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.100s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    size_t size = st.st_size;
    char *whole = whole_alloc(size);
    if (whole != NULL) {
        bool ok = read_all(f, whole, size);
        fclose(f);
        esp_err_t err = ok ? httpd_resp_send(req, whole, size)
                           : send_text(req, "500 Internal Server Error", "Can't read the SD card");
        heap_caps_free(whole);
        return err;
    }
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    if (buf == NULL) {
        fclose(f);
        return send_text(req, "500 Internal Server Error", "Out of memory");
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

/* ---- Long transfers in tasks of their own ---- */

static SemaphoreHandle_t s_transfers;

typedef struct {
    httpd_req_t *req;
    esp_err_t (*work)(httpd_req_t *req);
} transfer_t;

static void transfer_task(void *arg)
{
    transfer_t t = *(transfer_t *)arg;
    free(arg);
    t.work(t.req);
    httpd_req_async_handler_complete(t.req);
    xSemaphoreGive(s_transfers);
    vTaskDelete(NULL);
}

static esp_err_t start_transfer(httpd_req_t *req, esp_err_t (*work)(httpd_req_t *req))
{
    if (xSemaphoreTake(s_transfers, 0) != pdTRUE) {
        return send_text(req, "503 Service Unavailable", "Busy with other transfers: try again shortly");
    }
    transfer_t *t = malloc(sizeof(transfer_t));
    httpd_req_t *copy = NULL;
    if (t == NULL || httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        free(t);
        xSemaphoreGive(s_transfers);
        return work(req);       /* then here, blocking the server meanwhile */
    }
    t->req = copy;
    t->work = work;
    if (xTaskCreatePinnedToCore(transfer_task, "transfer", 6144, t, 5, NULL, 0) != pdPASS) {
        free(t);
        xSemaphoreGive(s_transfers);
        esp_err_t err = work(copy);
        httpd_req_async_handler_complete(copy);
        return err;
    }
    return ESP_OK;
}

static esp_err_t disk_put(httpd_req_t *req)
{
    return start_transfer(req, disk_put_work);
}

static esp_err_t disk_get(httpd_req_t *req)
{
    return start_transfer(req, disk_get_work);
}

static esp_err_t disk_delete(httpd_req_t *req)
{
    char name[100], path[160];
    if (!disk_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    if (disks_in_use(name)) {
        return send_conflict(req, name);
    }
    disk_path(path, sizeof(path), name);
    disks_removed(name);
    if (remove(path) != 0) {
        return send_text(req, "404 Not Found", "No such disk");
    }
    ESP_LOGI(TAG, "deleted %s", name);
    return send_text(req, "200 OK", "OK");
}

void emu_web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;
    config.stack_size = 6144;
    config.max_open_sockets = 5;
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    s_transfers = xSemaphoreCreateCounting(TRANSFERS, TRANSFERS);
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",               .method = HTTP_GET,    .handler = page_get },
        { .uri = "/api/info",       .method = HTTP_GET,    .handler = info_get },
        { .uri = "/machine.js",     .method = HTTP_GET,    .handler = script_get },
        { .uri = "/api/disks",      .method = HTTP_GET,    .handler = list_get },
        { .uri = "/api/floppies",   .method = HTTP_GET,    .handler = list_get },
        { .uri = "/api/status",     .method = HTTP_GET,    .handler = status_get },
        { .uri = "/api/drive/*",    .method = HTTP_PUT,    .handler = drive_put },
        { .uri = "/api/drive/*",    .method = HTTP_DELETE, .handler = drive_delete },
        { .uri = "/api/new",        .method = HTTP_POST,   .handler = new_post },
        { .uri = "/disks/*",        .method = HTTP_PUT,    .handler = disk_put },
        { .uri = "/disks/*",        .method = HTTP_POST,   .handler = disk_put },
        { .uri = "/disks/*",        .method = HTTP_GET,    .handler = disk_get },
        { .uri = "/disks/*",        .method = HTTP_DELETE, .handler = disk_delete },
        { .uri = "/floppies/*",     .method = HTTP_PUT,    .handler = disk_put },
        { .uri = "/floppies/*",     .method = HTTP_POST,   .handler = disk_put },
        { .uri = "/floppies/*",     .method = HTTP_GET,    .handler = disk_get },
        { .uri = "/floppies/*",     .method = HTTP_DELETE, .handler = disk_delete },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    if (mdns_init() == ESP_OK) {
        char service[64];
        mdns_hostname_set(CONFIG_EMU_HOSTNAME);
        mdns_instance_name_set(emu_machine()->name);
        snprintf(service, sizeof(service), "%s disks", emu_machine()->name);
        mdns_service_add(service, "_http", "_tcp", 80, NULL, 0);
#if CONFIG_EMUDBG
        /* The debugger (emudbg.h), for the VS Code extension to find */
        mdns_txt_item_t txt[] = { { "machine", emu_machine()->name } };
        snprintf(service, sizeof(service), "%s debugger", emu_machine()->name);
        mdns_service_add(service, "_esp32emu", "_tcp", CONFIG_EMUDBG_PORT, txt, 1);
#endif
    }
    ESP_LOGI(TAG, "web page on http://%s.local", CONFIG_EMU_HOSTNAME);
}
