/*
 * Tandy Color Computer disks.
 *
 * Disk BASIC (RS-DOS): tracks of 18 sectors of 256 bytes, the directory on
 * track 17, whose sector 2 is the granule table: a byte for each granule
 * (half a track, 9 sectors; track 17 has none), 0xFF free, else the next
 * granule of a file or 0xC0-0xC9 for its last. 68 granules on 35 tracks;
 * DOSes for 40 and 80 tracks use 78 and 158 (taken when the image has the
 * tracks and the longer table holds together).
 *
 * OS-9 / NitrOS-9: LSN 0 has DD.TOT (the sectors, 3 bytes at 0), DD.MAP
 * (the bitmap's bytes, 2 at 4), DD.BIT (sectors a cluster, 2 at 6) and
 * DD.DIR (the root directory, 3 at 8), big-endian. The bitmap follows from
 * LSN 1, a bit a cluster from bit 7 down, set: in use.
 */
#include "diskspace.h"

#define SECTOR      256
#define TRACK       (18 * SECTOR)
#define GRANULE     (9 * SECTOR)

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t be24(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

/* The first n granules' free ones, or -1 if the table doesn't hold
 * together (a granule pointing to itself, two to the same one)
 */
static int granules_free(const uint8_t *fat, int n)
{
    uint8_t seen[160 / 8] = { 0 };
    int free_granules = 0;
    for (int g = 0; g < n; g++) {
        uint8_t e = fat[g];
        if (e == 0xFF) {
            free_granules++;
        } else if (e < n) {
            if (e == g || (seen[e / 8] & (1u << (e & 7)))) {
                return -1;
            }
            seen[e / 8] |= (uint8_t)(1u << (e & 7));
        } else if (e < 0xC0 || e > 0xC9) {
            return -1;
        }
    }
    return free_granules;
}

bool diskspace_rsdos(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t fat[SECTOR];
    if ((d->size && d->size < 35 * TRACK) || !d->read(d->ctx, 17 * TRACK + SECTOR, fat, SECTOR)) {
        return false;
    }
    static const int tables[][2] = { { 80, 158 }, { 40, 78 }, { 35, 68 } };
    for (int i = 0; i < 3; i++) {
        int tracks = tables[i][0], n = tables[i][1];
        if (tracks > 35 && (d->size == 0 || d->size < (uint64_t)tracks * TRACK)) {
            continue;
        }
        int f = granules_free(fat, n);
        if (f >= 0) {
            out->free = (uint64_t)f * GRANULE;
            out->size = (uint64_t)n * GRANULE;
            return true;
        }
    }
    return false;
}

bool diskspace_os9(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t b[SECTOR];
    if (!d->read(d->ctx, 0, b, SECTOR)) {
        return false;
    }
    uint32_t tot = be24(b), map = be16(b + 4), bit = be16(b + 6), dir = be24(b + 8);
    uint32_t clusters = bit ? tot / bit : 0, map_sectors = (map + SECTOR - 1) / SECTOR;
    if (tot < 3 || bit == 0 || (bit & (bit - 1)) != 0 || map == 0 || (uint64_t)map * 8 * bit < tot ||
        map > (clusters + 7) / 8 + 1 || dir <= map_sectors || dir >= tot ||
        (d->size && (uint64_t)tot * SECTOR > d->size)) {
        return false;
    }
    uint32_t used = 0;
    for (uint32_t s = 0; s < map_sectors; s++) {
        if (!d->read(d->ctx, (uint64_t)(1 + s) * SECTOR, b, SECTOR)) {
            return false;
        }
        for (uint32_t i = 0; i < SECTOR; i++) {
            uint32_t first = (s * SECTOR + i) * 8;
            if (first >= clusters) {
                break;
            }
            uint8_t v = b[i];
            if (clusters - first < 8) {
                v &= (uint8_t)(0xFF00u >> (clusters - first));      /* the bits past the end */
            }
            used += (uint32_t)__builtin_popcount(v);
        }
    }
    out->free = (uint64_t)(clusters - used) * bit * SECTOR;
    out->size = (uint64_t)tot * SECTOR;
    return true;
}
