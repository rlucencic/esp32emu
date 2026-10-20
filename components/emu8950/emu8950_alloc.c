/*
 * emu8950's allocations in PSRAM where there is some (the S3's internal RAM
 * is short): CMakeLists.txt compiles emu8950.c and emuadpcm.c with calloc
 * and malloc renamed to these, so the library's own source stays as it is
 */
#include <stddef.h>
#include "esp_heap_caps.h"

void *emu8950_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

void *emu8950_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}
