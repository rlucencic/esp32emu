/*
 * CP/M 2.2 and 3: the directory (dir_entries x 32 bytes from the first
 * block after the reserved tracks) lists each file's allocation blocks,
 * 8-bit numbers when the disk has up to 256 blocks, else 16-bit (little
 * endian). Used: the directory's blocks and every block an entry names;
 * the rest is free. User numbers 0-15 are files; 0xE5 is a free entry,
 * CP/M 3's passwords (0x10-0x1F), label (0x20) and timestamps (0x21) have
 * no blocks.
 */
#include <stdlib.h>
#include "diskspace.h"

#define ENTRY           32
#define MAX_BLOCKS      65536

static bool pow2(uint32_t v)
{
    return v && (v & (v - 1)) == 0;
}

bool diskspace_cpm(const diskspace_disk_t *d, const diskspace_cpm_geometry_t *g, diskspace_t *out)
{
    if (!pow2(g->sector_size) || g->sector_size < 128 || g->sector_size > 8192 || g->sectors == 0 ||
        g->sectors > 255 || g->tracks <= g->reserved_tracks || g->tracks > 1024 || !pow2(g->block_size) ||
        g->block_size < 1024 || g->block_size > 16384 || g->dir_entries == 0 || g->dir_entries > 8192) {
        return false;
    }
    uint64_t start = (uint64_t)g->reserved_tracks * g->sectors * g->sector_size;
    uint64_t total = (uint64_t)(g->tracks - g->reserved_tracks) * g->sectors * g->sector_size / g->block_size;
    uint32_t dir_blocks = (g->dir_entries * ENTRY + g->block_size - 1) / g->block_size;
    if (total > MAX_BLOCKS || dir_blocks >= total) {
        return false;
    }
    uint8_t *used = calloc((total + 7) / 8, 1);
    if (used == NULL) {
        return false;
    }
    bool wide = total > 256, ok = true;
    for (uint32_t b = 0; b < dir_blocks; b++) {
        used[b / 8] |= 1u << (b & 7);
    }
    for (uint32_t i = 0; i < g->dir_entries && ok; i++) {
        uint8_t e[ENTRY];
        ok = d->read(d->ctx, start + (uint64_t)i * ENTRY, e, ENTRY);
        if (!ok || e[0] > 15) {
            continue;
        }
        /* a file's name is printable (bit 7: attributes), its record count up to 128 */
        for (int k = 1; k <= 11 && ok; k++) {
            ok = (e[k] & 0x7F) >= 0x20 && (e[k] & 0x7F) < 0x7F;
        }
        ok = ok && e[15] <= 0x80;
        for (int k = 0; k < (wide ? 8 : 16) && ok; k++) {
            uint32_t b = wide ? (uint32_t)(e[16 + 2 * k] | e[17 + 2 * k] << 8) : e[16 + k];
            if (b >= total) {
                ok = false;
            } else if (b) {
                used[b / 8] |= 1u << (b & 7);
            }
        }
    }
    uint32_t n_used = 0;
    for (uint32_t b = 0; b < total; b++) {
        n_used += (used[b / 8] >> (b & 7)) & 1;
    }
    free(used);
    if (!ok) {
        return false;
    }
    out->free = (total - n_used) * g->block_size;
    out->size = total * g->block_size;
    return true;
}
