#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs.h"
#include "emu/disks.h"
#include "emu/sdcard.h"

#define MAX_IMAGES  500

static const char *TAG = "disks";

static disks_config_t s_cfg;
static char s_folder[128];
static bool s_ready;
static char s_names[DISKS_MAX_DRIVES][DISKS_NAME_LEN];
static volatile uint8_t s_forget;       /* drives the machine ejected, to save */

static bool has_extension(const char *name, const char *ext)
{
    size_t len = strlen(name), elen = strlen(ext);
    return len > elen && strcasecmp(name + len - elen, ext) == 0;
}

static bool is_image(const char *name)
{
    if (name[0] == '.') {
        return false;
    }
    for (const char *const *e = s_cfg.extensions; *e; e++) {
        if (has_extension(name, *e)) {
            return true;
        }
    }
    return false;
}

static void remember(int drive)
{
    char key[8] = "disk_a";
    key[5] = 'a' + drive;
    nvs_handle_t h;
    if (nvs_open(s_cfg.nvs_namespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, s_names[drive]);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool disks_init(const disks_config_t *config)
{
    s_cfg = *config;
    if (s_cfg.drives > DISKS_MAX_DRIVES) {
        s_cfg.drives = DISKS_MAX_DRIVES;
    }
    snprintf(s_folder, sizeof(s_folder), "%s/%s", SDCARD_MOUNT_POINT, s_cfg.folder);
    struct stat st;
    if (stat(SDCARD_MOUNT_POINT, &st) != 0) {
        return false;
    }
    if (stat(s_folder, &st) != 0 && mkdir(s_folder, 0777) != 0) {
        ESP_LOGW(TAG, "can't make %s", s_folder);
        return false;
    }
    s_ready = true;
    return true;
}

const disks_config_t *disks_config(void)
{
    return &s_cfg;
}

bool disks_ready(void)
{
    return s_ready;
}

void disks_restore(void)
{
    nvs_handle_t h;
    if (!s_ready || nvs_open(s_cfg.nvs_namespace, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int d = 0; d < s_cfg.drives; d++) {
        char key[8] = "disk_a";
        key[5] = 'a' + d;
        char name[DISKS_NAME_LEN];
        size_t len = sizeof(name);
        if (nvs_get_str(h, key, name, &len) == ESP_OK && name[0]) {
            if (!disks_insert(d, name)) {
                ESP_LOGW(TAG, "%s %s is gone", s_cfg.drive_names[d], name);
            }
        }
    }
    nvs_close(h);
}

static int compare_names(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

int disks_list(char ***names)
{
    *names = NULL;
    DIR *dir = s_ready ? opendir(s_folder) : NULL;
    if (dir == NULL) {
        return 0;
    }
    char **list = malloc(MAX_IMAGES * sizeof(char *));
    int n = 0;
    struct dirent *e;
    while (list != NULL && n < MAX_IMAGES && (e = readdir(dir)) != NULL) {
        if (e->d_type != DT_DIR && is_image(e->d_name)) {
            list[n] = strdup(e->d_name);
            if (list[n] != NULL) {
                n++;
            }
        }
    }
    closedir(dir);
    if (list != NULL) {
        qsort(list, n, sizeof(char *), compare_names);
    }
    *names = list;
    return n;
}

void disks_free_list(char **names, int count)
{
    for (int i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

bool disks_insert(int drive, const char *name)
{
    if (!s_ready || drive < 0 || drive >= s_cfg.drives) {
        return false;
    }
    char path[sizeof(s_folder) + DISKS_NAME_LEN + 1];
    snprintf(path, sizeof(path), "%s/%s", s_folder, name);
    /* One image in one drive at a time: move it */
    for (int d = 0; d < s_cfg.drives; d++) {
        if (d != drive && disks_name(d) != NULL && strcasecmp(disks_name(d), name) == 0) {
            disks_eject(d);
        }
    }
    if (!s_cfg.insert(drive, path)) {
        return false;
    }
    snprintf(s_names[drive], DISKS_NAME_LEN, "%s", name);
    remember(drive);
    return true;
}

void disks_eject(int drive)
{
    if (drive < 0 || drive >= s_cfg.drives) {
        return;
    }
    s_cfg.eject(drive);
    s_names[drive][0] = 0;
    remember(drive);
}

void disks_ejected(int drive)
{
    if (drive >= 0 && drive < s_cfg.drives) {
        s_names[drive][0] = 0;
        __atomic_or_fetch(&s_forget, 1u << drive, __ATOMIC_RELAXED);
    }
}

void disks_poll(void)
{
    uint8_t forget = __atomic_exchange_n(&s_forget, 0, __ATOMIC_RELAXED);
    for (int d = 0; d < s_cfg.drives; d++) {
        if (forget & (1u << d)) {
            remember(d);
        }
    }
}

const char *disks_name(int drive)
{
    if (drive < 0 || drive >= s_cfg.drives || s_cfg.path(drive) == NULL) {
        return NULL;
    }
    return s_names[drive];
}

const char *disks_folder(void)
{
    return s_folder;
}

bool disks_valid_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= DISKS_NAME_LEN - 1 || !is_image(name)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = name[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", c) != NULL) {
            return false;
        }
    }
    return true;
}

bool disks_in_use(const char *name)
{
    for (int d = 0; d < s_cfg.drives; d++) {
        if (disks_name(d) != NULL && strcasecmp(disks_name(d), name) == 0) {
            return true;
        }
    }
    return false;
}

void disks_removed(const char *name)
{
    if (s_cfg.removed) {
        char path[sizeof(s_folder) + DISKS_NAME_LEN + 1];
        snprintf(path, sizeof(path), "%s/%s", s_folder, name);
        s_cfg.removed(path);
    }
}

/* <prefix>nnnn<ext>: one more than the highest number ever made (kept in
 * NVS, so deleting the newest image doesn't bring its name back) or in the
 * folder
 */
uint32_t disks_default_size(int drive)
{
    if (s_cfg.default_size) {
        return s_cfg.default_size(drive);
    }
    return s_cfg.size_count ? s_cfg.sizes[0].bytes : 0;
}

bool disks_create(int drive, uint32_t bytes, char *name, size_t len)
{
    if (!s_ready || s_cfg.create == NULL) {
        return false;
    }
    uint16_t last = 0;
    nvs_handle_t h;
    if (nvs_open(s_cfg.nvs_namespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "empty_n", &last);
        nvs_close(h);
    }
    size_t plen = strlen(s_cfg.new_prefix);
    DIR *dir = opendir(s_folder);
    struct dirent *e;
    while (dir != NULL && (e = readdir(dir)) != NULL) {
        unsigned n;
        if (strncasecmp(e->d_name, s_cfg.new_prefix, plen) == 0 &&
            sscanf(e->d_name + plen, "%4u", &n) == 1 && n > last) {
            last = n;
        }
    }
    if (dir != NULL) {
        closedir(dir);
    }
    if (last >= 9999) {
        return false;
    }

    const char *ext = s_cfg.extension_for ? s_cfg.extension_for(bytes) : s_cfg.new_extension;
    snprintf(name, len, "%s%04u%s", s_cfg.new_prefix, last + 1, ext);
    char path[sizeof(s_folder) + DISKS_NAME_LEN + 1];
    snprintf(path, sizeof(path), "%s/%s", s_folder, name);
    if (!s_cfg.create(path, bytes)) {
        return false;
    }
    if (nvs_open(s_cfg.nvs_namespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "empty_n", last + 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "made %s", name);
    return drive < 0 || disks_insert(drive, name);
}
