/*
 * Atari 8-bit disks: the free sectors their DOS counts.
 *
 * Atari DOS 2.0/2.5 and MyDOS: the VTOC in sector 360, byte 0 the DOS
 * code (2 for DOS 2; MyDOS more for its longer VTOCs), bytes 1-2 the
 * usable sectors, 3-4 the free ones (little-endian). DOS 2.5's enhanced
 * density (1040 sectors, 1010 usable) counts sectors 720-1023 in a second
 * VTOC, sector 1024 (its free ones in bytes 122-123).
 *
 * SpartaDOS: the boot sector (sector 1) jumps to $3080 (0x80 in byte 7),
 * has the disk's sectors in bytes 11-12 and the free ones in 13-14, the
 * sector size in 31 (0x80: 128, 0: 256) and the version in 32 (0x11, 0x2x).
 *
 * Bytes are sectors x sector size (DOS 2 keeps 125 or 253 of them for a
 * file's data, the rest links).
 */
#include "diskspace.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Sector n's offset: a 256-byte disk's first three are 128 */
static uint64_t offset(uint32_t n, uint32_t sector_size)
{
    return sector_size == 128 || n <= 3 ? (uint64_t)(n - 1) * 128 : (uint64_t)(n - 1) * sector_size - 384;
}

static bool read_sector(const diskspace_disk_t *d, uint32_t n, uint32_t sector_size, uint8_t *buf)
{
    return d->read(d->ctx, offset(n, sector_size), buf, n <= 3 ? 128 : sector_size);
}

static bool sparta(const diskspace_disk_t *d, uint32_t sector_size, uint32_t sectors, diskspace_t *out)
{
    uint8_t b[128];
    if (!read_sector(d, 1, sector_size, b) || b[7] != 0x80 || (b[32] != 0x11 && (b[32] & 0xF0) != 0x20) ||
        b[31] != (sector_size == 128 ? 0x80 : 0x00)) {
        return false;
    }
    uint32_t total = le16(b + 11), free_sectors = le16(b + 13);
    if (total < 4 || free_sectors > total || (sectors && total > sectors)) {
        return false;
    }
    out->free = (uint64_t)free_sectors * sector_size;
    out->size = (uint64_t)total * sector_size;
    return true;
}

static bool dos2(const diskspace_disk_t *d, uint32_t sector_size, uint32_t sectors, diskspace_t *out)
{
    uint8_t v[256];
    if (sectors && sectors < 368) {
        return false;
    }
    if (!read_sector(d, 360, sector_size, v) || v[0] < 2 || v[0] > 0x40) {
        return false;
    }
    uint32_t total = le16(v + 1), free_sectors = le16(v + 3);
    if (total == 0 || free_sectors > total || (sectors && (total > sectors || 2 * total < sectors))) {
        return false;
    }
    if (v[0] == 2 && sector_size == 128 && total == 1010) {
        /* DOS 2.5 enhanced density: sector 360 counts sectors up to 719 */
        uint8_t v2[128];
        if (!read_sector(d, 1024, 128, v2) || free_sectors > 707 || le16(v2 + 122) > 304) {
            return false;
        }
        free_sectors += le16(v2 + 122);
        if (free_sectors > total) {
            return false;
        }
    }
    out->free = (uint64_t)free_sectors * sector_size;
    out->size = (uint64_t)total * sector_size;
    return true;
}

bool diskspace_atari(const diskspace_disk_t *d, uint32_t sector_size, diskspace_t *out)
{
    if (sector_size != 128 && sector_size != 256) {
        return false;
    }
    /* the image's sectors (0: not known) */
    uint32_t sectors = 0;
    if (sector_size == 128) {
        sectors = (uint32_t)(d->size / 128);
    } else if (d->size >= 384) {
        sectors = (uint32_t)(3 + (d->size - 384) / 256);
    }
    if (d->size && sectors < 4) {
        return false;
    }
    return sparta(d, sector_size, sectors, out) || dos2(d, sector_size, sectors, out);
}
