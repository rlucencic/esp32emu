# The emulator runtime for the Makerfabs MaTouch ESP32-S3 7" (see include/emu/emu.h)
set(srcs "emu.c" "console.c" "disks.c" "json.c" "lcd.c" "sdcard.c" "touch.c")
set(embed "")
if(CONFIG_EMU_WIFI)
    list(APPEND srcs "net.c" "setup_ui.c" "lv_mem_psram.c")
endif()
if(CONFIG_EMU_BLE_HID)
    list(APPEND srcs "ble_hid.c")
endif()
if(CONFIG_EMU_SPEAKER)
    list(APPEND srcs "speaker.c")
endif()
if(CONFIG_EMU_WEB)
    list(APPEND srcs "web.c")
    list(APPEND embed "web/index.html")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       EMBED_FILES ${embed}
                       REQUIRES esp_driver_gpio esp_timer
                       PRIV_REQUIRES esp_psram esp_partition esp_lcd
                                     esp_driver_usb_serial_jtag vfs nvs_flash bt fatfs sdmmc
                                     esp_driver_sdmmc esp_driver_i2s esp_driver_uart esp_driver_i2c
                                     esp_wifi esp_netif esp_event lwip esp_http_server esp_driver_tsens
                                     esp_driver_gptimer)

if(CONFIG_EMU_WIFI)
    # LVGL calls lv_mem_psram.c's allocator: make the linker take it from here
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
endif()
