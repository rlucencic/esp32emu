/* The tests' helpers: an image in memory or from a file, checks */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diskspace.h"

typedef struct {
    uint8_t *data;
    uint64_t size;
} test_image_t;

static bool test_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    const test_image_t *im = ctx;
    if (offset > im->size || len > im->size - offset) {
        return false;
    }
    memcpy(buf, im->data + offset, len);
    return true;
}

static inline diskspace_disk_t test_disk(test_image_t *im)
{
    diskspace_disk_t d = { test_read, im, im->size };
    return d;
}

/* A file's bytes (exits if it can't) */
static inline test_image_t test_load(const char *path)
{
    test_image_t im = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    im.size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    im.data = malloc(im.size ? im.size : 1);
    if (fread(im.data, 1, im.size, f) != im.size) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return im;
}

static int test_failures;

#define TEST_CHECK(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
            test_failures++;                                                                  \
        }                                                                                     \
    } while (0)
