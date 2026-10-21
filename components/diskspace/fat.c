/*
 * FAT12, FAT16 and FAT32: MS-DOS and the PC, the Atari ST, MSX-DOS and
 * Nextor.
 *
 * The boot sector's BPB gives the layout: reserved sectors, the FATs, the
 * root directory (none on FAT32), then the data area's clusters, numbered
 * from 2. The cluster count alone decides the FAT's kind (under 4085
 * FAT12, under 65525 FAT16, else FAT32, as Microsoft's spec has it); a
 * free cluster's entry is 0. FAT32 keeps a free count in its FSInfo sector,
 * taken when its signatures are there and it fits. Atari ST boot sectors
 * have no 0x55AA and often no jump: a sane BPB is enough.
 *
 * The size is the data area's, clusters x cluster bytes: the "bytes total"
 * DOS's CHKDSK and the ST's Dfree() report.
 */
#include "diskspace.h"

#define SECTOR  512                 /* FAT reads, and an MBR's sectors */

static uint32_t le16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool pow2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* The entries for clusters 2 .. clusters + 1 that are 0, the FAT (at fat)
 * read a sector at a time into b (FAT12's entries cross sectors: bytes go
 * in threes, two entries)
 */
static bool count_free(const diskspace_disk_t *d, uint64_t fat, int bits, uint32_t clusters, uint8_t *b,
                       uint32_t *n_free)
{
    uint64_t entries = (uint64_t)clusters + 2, bytes = (entries * bits + 7) / 8;
    uint32_t n = 0, e = 0;
    uint8_t pair[3];
    int have = 0;
    for (uint64_t at = 0; at < bytes; at += SECTOR) {
        uint32_t len = bytes - at < SECTOR ? (uint32_t)(bytes - at) : SECTOR;
        if (!d->read(d->ctx, fat + at, b, len)) {
            return false;
        }
        if (bits == 12) {
            for (uint32_t i = 0; i < len; i++) {
                pair[have++] = b[i];
                if (have == 3) {
                    if (e >= 2) {
                        n += (pair[0] | (pair[1] & 0x0F) << 8) == 0;
                        n += (pair[1] >> 4 | pair[2] << 4) == 0;
                    }
                    e += 2;
                    have = 0;
                }
            }
        } else {
            for (uint32_t i = 0; i < len; i += bits / 8, e++) {
                uint32_t v = bits == 16 ? le16(b + i) : le32(b + i) & 0x0FFFFFFF;
                n += e >= 2 && v == 0;
            }
        }
    }
    if (have == 2 && e >= 2) {
        n += (pair[0] | (pair[1] & 0x0F) << 8) == 0;    /* FAT12's last, an even one */
    }
    *n_free = n;
    return true;
}

bool diskspace_fat(const diskspace_disk_t *d, uint64_t base, diskspace_t *out)
{
    uint8_t b[SECTOR];
    if (!d->read(d->ctx, base, b, SECTOR)) {
        return false;
    }
    uint32_t bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16];
    uint32_t root_entries = le16(b + 17), total = le16(b + 19), fat_sectors = le16(b + 22);
    bool fat32_bpb = fat_sectors == 0;      /* FAT32's sizes are 32-bit */
    if (total == 0) {
        total = le32(b + 32);
    }
    if (fat32_bpb) {
        fat_sectors = le32(b + 36);
    }
    if (bps < 512 || bps > 4096 || !pow2(bps) || !pow2(spc) || reserved == 0 || fats < 1 || fats > 2 ||
        total == 0 || fat_sectors == 0) {
        return false;
    }
    if (d->size && (base >= d->size || total > (d->size - base) / bps)) {
        return false;                       /* more sectors than the image */
    }
    uint64_t first_data = reserved + (uint64_t)fats * fat_sectors + (root_entries * 32 + bps - 1) / bps;
    if (first_data >= total) {
        return false;
    }
    uint32_t clusters = (total - (uint32_t)first_data) / spc;
    int bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if (clusters == 0 || (bits == 32 ? root_entries != 0 || !fat32_bpb : root_entries == 0) ||
        (uint64_t)fat_sectors * bps * 8 / bits < (uint64_t)clusters + 2) {
        return false;                       /* not this kind, or its FAT is too small */
    }
    uint32_t info = le16(b + 48), n_free;
    bool counted = false;
    if (bits == 32 && info >= 1 && info < reserved && d->read(d->ctx, base + (uint64_t)info * bps, b, SECTOR) &&
        le32(b) == 0x41615252 && le32(b + 484) == 0x61417272 && le32(b + 508) == 0xAA550000 &&
        le32(b + 488) <= clusters) {
        n_free = le32(b + 488);             /* FSInfo's (0xFFFFFFFF: not known) */
        counted = true;
    }
    if (!counted && !count_free(d, base + (uint64_t)reserved * bps, bits, clusters, b, &n_free)) {
        return false;
    }
    out->free = (uint64_t)n_free * bps * spc;
    out->size = (uint64_t)clusters * bps * spc;
    return true;
}

/* ---- Hard disks ---- */

static bool fat_type(uint8_t type)
{
    switch (type) {
    case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:  /* FAT12, 16, 32 */
    case 0x11: case 0x14: case 0x16: case 0x1B: case 0x1C: case 0x1E:  /* hidden */
        return true;
    default:
        return false;
    }
}

static bool extended_type(uint8_t type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* The partition table of the sector at lba (an MBR, an EBR): its four
 * 16-byte entries and the 0x55AA after them; false without that, or with a
 * boot flag other than 0 and 0x80 (then it's code: a boot sector's)
 */
static bool read_table(const diskspace_disk_t *d, uint64_t lba, uint8_t t[66])
{
    if (!d->read(d->ctx, lba * SECTOR + 446, t, 66) || t[64] != 0x55 || t[65] != 0xAA) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (t[16 * i] & 0x7F) {
            return false;
        }
    }
    return true;
}

static void add(const diskspace_disk_t *d, uint64_t lba, diskspace_t *sum, bool *any)
{
    diskspace_t s;
    if (diskspace_fat(d, lba * SECTOR, &s)) {
        sum->free += s.free;
        sum->size += s.size;
        *any = true;
    }
}

bool diskspace_fat_disk(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t t[66], e[66];
    diskspace_t sum = { 0, 0 };
    bool any = false;
    if (read_table(d, 0, t)) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *p = t + 16 * i;
            uint32_t start = le32(p + 8);
            if (start == 0 || le32(p + 12) == 0) {
                continue;
            }
            if (fat_type(p[4])) {
                add(d, start, &sum, &any);
            } else if (extended_type(p[4])) {
                /* Its logical partitions: a chain of EBRs, each with one
                 * (from the EBR) and the next EBR (from the extended start)
                 */
                uint64_t ebr = start;
                for (int n = 0; n < 64 && read_table(d, ebr, e); n++) {
                    if (fat_type(e[4]) && le32(e + 8) != 0) {
                        add(d, ebr + le32(e + 8), &sum, &any);
                    }
                    if (!extended_type(e[20]) || le32(e + 24) == 0) {
                        break;
                    }
                    ebr = start + (uint64_t)le32(e + 24);
                }
            }
        }
        if (any) {
            *out = sum;
            return true;
        }
    }
    return diskspace_fat(d, 0, out);        /* a floppy, a "superfloppy" */
}
