/*
 * Emulated sound -> the board's speaker, 16-bit stereo.
 * MaTouch: MAX98357A over I2S (the amp mixes L/R).
 * Tab5: the ES8388 codec and NS4150 amp through the BSP (esp_codec_dev).
 * The I2S clock is the master: this task drains the machine's sound ring
 * at exactly its rate, and the emulator paces itself on the ring's level.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emu/board.h"
#include "emu/speaker.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#else
#include "driver/i2s_std.h"
#endif

#define CHUNK_FRAMES    256
#if CONFIG_IDF_TARGET_ESP32P4
#define STACK           4096    /* esp_codec_dev's write path */
#else
#define STACK           3072
#endif

static const char *TAG = "speaker";
static speaker_render_t s_render;
static int32_t s_volume;

#if CONFIG_IDF_TARGET_ESP32P4

static esp_codec_dev_handle_t s_codec;

static bool output_start(int rate)
{
    if (bsp_audio_init(NULL) != ESP_OK || (s_codec = bsp_audio_codec_speaker_init()) == NULL) {
        ESP_LOGE(TAG, "no audio codec");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = rate,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }
    /* Full scale in the codec; the volume setting scales the samples */
    esp_codec_dev_set_out_vol(s_codec, 100);
    return true;
}

static void output_write(const int16_t *buf, size_t bytes)
{
    esp_codec_dev_write(s_codec, (void *)buf, bytes);
}

#else

static i2s_chan_handle_t s_tx;

static bool output_start(int rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = CHUNK_FRAMES;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "no I2S channel");
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_GPIO_BCLK,
            .ws = BOARD_I2S_GPIO_LRCLK,
            .dout = BOARD_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed");
        return false;
    }
    return true;
}

static void output_write(const int16_t *buf, size_t bytes)
{
    size_t written;
    i2s_channel_write(s_tx, buf, bytes, &written, portMAX_DELAY);
}

#endif

static void speaker_task(void *arg)
{
    static int16_t buf[CHUNK_FRAMES * 2];

    for (;;) {
        int n = s_render(buf, CHUNK_FRAMES);
        /* Underrun: hold the last sample rather than click to zero */
        for (int i = n; i < CHUNK_FRAMES; i++) {
            buf[i * 2] = n ? buf[(n - 1) * 2] : 0;
            buf[i * 2 + 1] = n ? buf[(n - 1) * 2 + 1] : 0;
        }
        for (int i = 0; i < CHUNK_FRAMES * 2; i++) {
            buf[i] = (int32_t)buf[i] * s_volume >> 16;
        }
        output_write(buf, sizeof(buf));
    }
}

bool speaker_start(int rate, speaker_render_t render, int volume)
{
    s_render = render;
    s_volume = volume * 65536 / 100;
    if (!output_start(rate)) {
        return false;
    }
    xTaskCreatePinnedToCore(speaker_task, "speaker", STACK, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "speaker on, %d Hz", rate);
    return true;
}
