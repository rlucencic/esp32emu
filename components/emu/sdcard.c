/*
 * microSD card on the SDMMC peripheral, FAT with long file names.
 * MaTouch: 1-bit bus (pins from board.h, as in wmbus_hub).
 * Tab5: the BSP's slot 0, 4-bit; the C6 has slot 1.
 */
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "emu/board.h"
#include "emu/sdcard.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "bsp/m5stack_tab5.h"
#endif

static const char *TAG = "sdcard";
static bool s_mounted;

#if CONFIG_IDF_TARGET_ESP32P4
/* A failed mount deinitialises its slot, and the SDMMC host with the last
 * slot: esp_hosted (the C6, slot 1) has started the host by then but not
 * yet its slot, and its link broke ("Unrecoverable host sdio state", a
 * restart every few seconds). Leave the host alone.
 */
static esp_err_t keep_host(int slot)
{
    return ESP_OK;
}

/* The driver's bounce buffer, reserved once in PSRAM. FATFS's sector
 * windows and file caches aren't aligned to the cache line, so most
 * transfers go through one; without it the driver allocates internal DMA
 * memory for each (8 KB, halved down to a sector) and fails outright when
 * none is left: every upload failed while an A1200 ran (esp32ami, internal
 * free down to 29 KB). The P4's SDMMC reads and writes PSRAM directly once
 * the buffer and its length are aligned to the cache line; the driver takes
 * the length from heap_caps_get_allocated_size(), so that is checked too.
 */
#define BOUNCE_ALIGN    128             /* the L2 cache line, 64 or 128 */
static void *bounce_buffer(const sdmmc_host_t *host)
{
    size_t sectors = host->unaligned_multi_block_rw_max_chunk_size;
    size_t size = (sectors ? sectors : 1) * 512;
    void *buf = heap_caps_aligned_alloc(BOUNCE_ALIGN, size, MALLOC_CAP_SPIRAM);
    if (buf != NULL && heap_caps_get_allocated_size(buf) % BOUNCE_ALIGN != 0) {
        heap_caps_free(buf);
        buf = NULL;
    }
    if (buf == NULL) {
        ESP_LOGW(TAG, "no bounce buffer: transfers allocate their own");
    }
    return buf;
}
#endif

bool sdcard_mount(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    /* Not the BSP's mount: it powers the card through the chip's LDO 4,
     * which the Tab5 doesn't need (esp32_basic, wmbus_hub)
     */
    sdmmc_host_t host = { 0 };
    sdmmc_slot_config_t slot = { 0 };
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    host.pwr_ctrl_handle = NULL;
    host.flags |= SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit_p = keep_host;
    host.dma_aligned_buffer = bounce_buffer(&host);
#else
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    /* S3 cannot DMA from PSRAM. Reserve a sector buffer at mount time;
     * Wi-Fi and the emulator can leave too little DMA memory for later I/O.
     */
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_GPIO_CLK;
    slot.cmd = BOARD_SD_GPIO_CMD;
    slot.d0 = BOARD_SD_GPIO_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
#if CONFIG_IDF_TARGET_ESP32P4
        heap_caps_free(host.dma_aligned_buffer);
#endif
        return false;
    }
    ESP_LOGI(TAG, "%s: %llu MB", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    s_mounted = true;
    return true;
}

bool sdcard_space(uint64_t *total, uint64_t *free_bytes)
{
    return s_mounted && esp_vfs_fat_info(SDCARD_MOUNT_POINT, total, free_bytes) == ESP_OK;
}
