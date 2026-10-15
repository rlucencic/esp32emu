#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SDCARD_MOUNT_POINT  "/sdcard"

/* Mount the microSD card (SDMMC, 1-bit) at SDCARD_MOUNT_POINT */
bool sdcard_mount(void);

/* Card size and free space in bytes; false without a card */
bool sdcard_space(uint64_t *total, uint64_t *free_bytes);
