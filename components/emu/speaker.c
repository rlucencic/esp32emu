/*
 * Emulated sound -> MAX98357A over I2S (16-bit stereo; the amp mixes L/R).
 * The I2S clock is the master: this task drains the machine's sound ring
 * at exactly its rate, and the emulator paces itself on the ring's level.
 */
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emu/board.h"
#include "emu/speaker.h"

#define CHUNK_FRAMES    256

static const char *TAG = "speaker";
static i2s_chan_handle_t s_tx;
static speaker_render_t s_render;
static int32_t s_volume;

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
        size_t written;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

bool speaker_start(int rate, speaker_render_t render, int volume)
{
    s_render = render;
    s_volume = volume * 65536 / 100;
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
    xTaskCreatePinnedToCore(speaker_task, "speaker", 3072, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "speaker on, %d Hz", rate);
    return true;
}
