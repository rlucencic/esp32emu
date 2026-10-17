/*
 * M5Stack Tab5 display: the BSP's MIPI-DSI panel (ILI9881C, ST7123 or
 * ST7121, it finds out which), 720x1280 portrait with its framebuffer in
 * PSRAM. The runtime draws a 1280x720 landscape picture; lcd_flush() has
 * the PPA turn the changed rows by 90 degrees into the panel's framebuffer.
 */
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "emu/board.h"
#include "emu/lcd.h"

#define ST712X_TOUCH_ADDR   0x55
#define LANE_MBPS           1000    /* the BSP's; its ST7121 boards: 965 */
#define LANE_MBPS_ST7121    965

static const char *TAG = "lcd";

static uint16_t *s_picture;         /* landscape, what the runtime draws */
static uint16_t *s_panel_fb;        /* portrait, what the panel shows */
static ppa_client_handle_t s_ppa;
static SemaphoreHandle_t s_lock;

/* The BSP drives ST7121 panels (touch firmware 1 at 0x55) a bit slower */
static bool is_st7121(void)
{
    if (bsp_feature_enable(BSP_FEATURE_TOUCH, true) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (i2c_master_probe(bus, ST712X_TOUCH_ADDR, 100) != ESP_OK) {
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ST712X_TOUCH_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return false;
    }
    const uint8_t reg[2] = { 0, 0 };
    uint8_t version = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, reg, sizeof(reg), &version, 1, 100);
    i2c_master_bus_rm_device(dev);
    return err == ESP_OK && version == 1;
}

/* Landscape rows y0 .. y1 - 1 become panel columns y0 .. y1 - 1: a quarter
 * turn counter-clockwise, (x, y) -> (y, 1279 - x)
 */
static esp_err_t rotate(int y0, int y1)
{
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = s_picture,
            .pic_w = BOARD_LCD_H_RES,
            .pic_h = BOARD_LCD_V_RES,
            .block_w = BOARD_LCD_H_RES,
            .block_h = y1 - y0,
            .block_offset_x = 0,
            .block_offset_y = y0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_panel_fb,
            .buffer_size = BOARD_PANEL_W * BOARD_PANEL_H * sizeof(uint16_t),
            .pic_w = BOARD_PANEL_W,
            .pic_h = BOARD_PANEL_H,
            .block_offset_x = y0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

void lcd_flush(int y0, int y1)
{
    if (s_ppa == NULL) {
        return;
    }
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > BOARD_LCD_V_RES ? BOARD_LCD_V_RES : y1;
    if (y0 >= y1) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rotate(y0, y1);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPA rotate failed: %s", esp_err_to_name(err));
    }
}

/* The corners land where the orientation above says */
static bool rotation_works(void)
{
    const uint16_t red = 0xF800, green = 0x07E0, blue = 0x001F;
    s_picture[0] = red;                                         /* top left */
    s_picture[BOARD_LCD_H_RES - 1] = green;                     /* top right */
    s_picture[(BOARD_LCD_V_RES - 1) * BOARD_LCD_H_RES] = blue;  /* bottom left */
    bool ok = rotate(0, BOARD_LCD_V_RES) == ESP_OK &&
              s_panel_fb[(BOARD_PANEL_H - 1) * BOARD_PANEL_W] == red &&
              s_panel_fb[0] == green &&
              s_panel_fb[(BOARD_PANEL_H - 1) * BOARD_PANEL_W + BOARD_PANEL_W - 1] == blue;
    memset(s_picture, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
    rotate(0, BOARD_LCD_V_RES);
    return ok;
}

uint16_t *lcd_init(void)
{
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed");
        return NULL;
    }
    const bsp_display_config_t cfg = {
        .dsi_bus = {
            .phy_clk_src = 0,       /* the driver's default */
            .lane_bit_rate_mbps = is_st7121() ? LANE_MBPS_ST7121 : LANE_MBPS,
        },
    };
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    void *fb = NULL;
    esp_err_t err = bsp_display_new(&cfg, &panel, &io);
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MIPI-DSI panel failed: %s", esp_err_to_name(err));
        return NULL;
    }
    s_panel_fb = fb;

    size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t);
    s_picture = heap_caps_aligned_calloc(128, 1, bytes, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    const ppa_client_config_t ppa = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (s_picture == NULL || ppa_register_client(&ppa, &s_ppa) != ESP_OK) {
        ESP_LOGE(TAG, "no picture buffer or PPA");
        return NULL;
    }
    if (!rotation_works()) {
        ESP_LOGW(TAG, "PPA rotation is not the expected one: the picture will be turned");
    }

    bsp_display_brightness_set(BOARD_LCD_BRIGHTNESS);
    ESP_LOGI(TAG, "MIPI-DSI panel %dx%d ready, shown as %dx%d", BOARD_PANEL_W, BOARD_PANEL_H,
             BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return s_picture;
}
