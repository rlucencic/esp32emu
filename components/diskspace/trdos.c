/*
 * TR-DOS: the system sector (logical sector 8: track 0's ninth) keeps the
 * disk's type at 0xE3 (0x16 80 tracks two sides, 0x17 40 two, 0x18 80
 * one, 0x19 40 one), its free sectors at 0xE5 (16-bit, little endian:
 * those after the last file, as CAT shows them) and TR-DOS's ID 0x10 at
 * 0xE7. Track 0 is the catalogue; the rest is for files.
 */
#include "diskspace.h"

#define SECTOR          256

bool diskspace_trdos(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t s[SECTOR];
    if (!d->read(d->ctx, 8 * SECTOR, s, SECTOR) || s[0xE7] != 0x10) {
        return false;
    }
    uint32_t tracks, sides;
    switch (s[0xE3]) {
    case 0x16: tracks = 80; sides = 2; break;
    case 0x17: tracks = 40; sides = 2; break;
    case 0x18: tracks = 80; sides = 1; break;
    case 0x19: tracks = 40; sides = 1; break;
    default: return false;
    }
    uint32_t data = tracks * sides * 16 - 16, free_sectors = s[0xE5] | s[0xE6] << 8;
    if (free_sectors > data) {
        return false;
    }
    out->free = (uint64_t)free_sectors * SECTOR;
    out->size = (uint64_t)data * SECTOR;
    return true;
}
