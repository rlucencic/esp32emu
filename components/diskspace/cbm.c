/*
 * Commodore disks: the free blocks the BAM counts, added up as the drive
 * does for "blocks free" (the directory track left out).
 *
 * D64 (1541): the BAM in track 18 sector 0, a track's free count in byte
 * 4 * t (t 1-35). 40-track images may have tracks 36-40 too: SpeedDOS
 * keeps them at 0xC0, DolphinDOS at 0xAC (4 bytes each; taken when they
 * agree with their bitmaps). D71 (1571): side 2's counts (tracks 36-70) at
 * 0xDD when byte 3 says double-sided; its track 53 holds the second BAM
 * and is left out too. D81 (1581): the header in track 40 sector 0, the
 * BAM in sectors 1 and 2 (40 tracks each, 6 bytes a track from 0x10, the
 * count first), track 40 left out.
 *
 * A block is 256 bytes here (254 of them a file's data): the drive's
 * blocks, as the directory shows them.
 */
#include "diskspace.h"

#define BLOCK       256
#define D64_BAM     (357 * BLOCK)       /* track 18 sector 0 */
#define D71_BLOCKS  1366                /* a side 683 */

/* A 1541 track's sectors (a D64's tracks 36-40 have 17) */
static int sectors(int track)
{
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

static int bits(const uint8_t *p, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        c += __builtin_popcount(p[i]);
    }
    return c;
}

/* Tracks 36-40's entries at bam + base (SpeedDOS, DolphinDOS): their free
 * blocks, or -1 if they aren't there (each count must match its bitmap)
 */
static int extra_tracks(const uint8_t *bam, int base)
{
    int free_blocks = 0, used = 0;
    for (int t = 0; t < 5; t++) {
        const uint8_t *e = bam + base + 4 * t;
        if (e[0] > 17 || (e[3] & 0xFE) != 0 || bits(e + 1, 3) != e[0]) {
            return -1;
        }
        free_blocks += e[0];
        used |= e[0] | e[1] | e[2] | e[3];
    }
    return used ? free_blocks : -1;
}

static bool d64(const diskspace_disk_t *d, bool forty, bool d71, diskspace_t *out)
{
    /* formatted: the link to the directory, or the DOS version 'A' (C128
     * CP/M's disks link elsewhere)
     */
    uint8_t bam[BLOCK];
    if (!d->read(d->ctx, D64_BAM, bam, BLOCK) || !((bam[0] == 18 && bam[1] < 19) || bam[2] == 'A')) {
        return false;
    }
    uint32_t free_blocks = 0, blocks = 0;
    for (int t = 1; t <= 35; t++) {
        if (bam[4 * t] > sectors(t)) {
            return false;
        }
        if (t != 18) {
            free_blocks += bam[4 * t];
            blocks += sectors(t);
        }
    }
    if (d71 && (bam[3] & 0x80)) {
        for (int t = 36; t <= 70; t++) {
            uint8_t n = bam[0xDD + t - 36];
            if (n > sectors(t - 35)) {
                return false;
            }
            if (t != 53) {
                free_blocks += n;
                blocks += sectors(t - 35);
            }
        }
    }
    if (forty) {
        int extra = extra_tracks(bam, 0xC0);
        if (extra < 0) {
            extra = extra_tracks(bam, 0xAC);
        }
        if (extra >= 0) {
            free_blocks += extra;
            blocks += 5 * 17;
        }
    }
    out->free = (uint64_t)free_blocks * BLOCK;
    out->size = (uint64_t)blocks * BLOCK;
    return true;
}

static bool d81(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t b[BLOCK];
    const uint64_t header = 39 * 40 * BLOCK;    /* track 40 sector 0 */
    if (!d->read(d->ctx, header, b, BLOCK) || b[2] != 'D') {
        return false;
    }
    uint32_t free_blocks = 0;
    for (int s = 1; s <= 2; s++) {
        if (!d->read(d->ctx, header + s * BLOCK, b, BLOCK) || b[2] != 'D' || b[3] != 0xBB) {
            return false;
        }
        for (int i = 0; i < 40; i++) {
            int t = (s - 1) * 40 + i + 1;
            uint8_t n = b[0x10 + 6 * i];
            if (n > 40) {
                return false;
            }
            if (t != 40) {
                free_blocks += n;
            }
        }
    }
    out->free = (uint64_t)free_blocks * BLOCK;
    out->size = (uint64_t)79 * 40 * BLOCK;
    return true;
}

bool diskspace_cbm(const diskspace_disk_t *d, diskspace_t *out)
{
    switch (d->size) {
    case 174848:                        /* 35 tracks (+ 683 error bytes) */
    case 175531:
        return d64(d, false, false, out);
    case 196608:                        /* 40 tracks (+ 768) */
    case 197376:
        return d64(d, true, false, out);
    case D71_BLOCKS * BLOCK:            /* (+ 1366) */
    case D71_BLOCKS * (BLOCK + 1):
        return d64(d, false, true, out);
    case 819200:                        /* (+ 3200) */
    case 822400:
        return d81(d, out);
    default:
        return false;
    }
}
