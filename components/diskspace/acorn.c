/*
 * ADFS / FileCore discs (Acorn Archimedes; the BBC Master's ADFS too).
 *
 * The old map (S, M, L floppies of 256-byte sectors, D of 1024): the first
 * 512 bytes, two map sectors of 256. The first holds the free areas'
 * starts (3 bytes each, in 256-byte units) and at &FC the disc's size in
 * them; the second their lengths and at &FE the list's end (3 x areas);
 * each ends with a check byte. The root directory ("Hugo") follows at &200
 * (at &400 on a D disc). Only the first track is read: an L disc's sides,
 * in whatever order, don't matter.
 *
 * The new map (E, F floppies, hard discs): a disc record (in the boot block
 * at &C00 + &1C0; 4 bytes into the map's first zone on an E floppy) gives
 * its layout: nzones zones of a sector each, twice, where the middle zone's
 * part of the disc starts (the disc's start with one zone). A zone: a check
 * byte, the free link (15 bits at bit 8), the cross check, then fragments,
 * each idlen bits of id, zeros and a 1 bit, every bit 1 << log2bpmb bytes
 * of the disc; zone 0's after a copy of the disc record (60 bytes). A
 * zone's free fragments are chained through their id bits: the distance in
 * bits to the next one (0: the last).
 */
#include <string.h>
#include "diskspace.h"

#define DR_BITS         (60 * 8)        /* the disc record in zone 0 */
#define BOOT_BLOCK      0xC00
#define LOG2_SECTOR_MAX 10              /* sectors up to 1 KB: floppies, IDE and SCSI discs */
#define F_MAP           0xC6800         /* an F floppy's map: 4 zones of 6592 bits, 64 bytes a bit */

static uint32_t le24(const uint8_t *p)
{
    return p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
}

static uint32_t le32(const uint8_t *p)
{
    return le24(p) | (uint32_t)p[3] << 24;
}

/* Bytes n-1 down to 0 added with their carries, from sum: the old map's
 * check bytes (from 255) and the boot block's (from 0)
 */
static uint8_t sum_check(const uint8_t *b, int n, uint32_t sum)
{
    for (int i = n - 1; i >= 0; i--) {
        sum = (sum & 0xFF) + (sum >> 8) + b[i];
    }
    return (uint8_t)sum;
}

/* ---- The old map ---- */

static bool old_map(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t map[512], dir[5];
    if (!d->read(d->ctx, 0, map, sizeof(map)) || sum_check(map, 255, 255) != map[255] ||
        sum_check(map + 256, 255, 255) != map[511]) {
        return false;
    }
    uint32_t sectors = le24(map + 0xFC), end = map[0x1FE];
    if (sectors < 8 || end % 3 != 0 || end > 82 * 3) {
        return false;
    }
    if (!(d->read(d->ctx, 0x200, dir, 5) && memcmp(dir + 1, "Hugo", 4) == 0) &&
        !(d->read(d->ctx, 0x400, dir, 5) && memcmp(dir + 1, "Hugo", 4) == 0)) {
        return false;
    }
    uint64_t free_sectors = 0;
    for (uint32_t i = 0; i < end; i += 3) {
        uint32_t start = le24(map + i), len = le24(map + 256 + i);
        if (start < 2 || start + len > sectors) {
            return false;
        }
        free_sectors += len;
    }
    if (free_sectors > sectors) {
        return false;
    }
    out->free = free_sectors * 256;
    out->size = (uint64_t)sectors * 256;
    return true;
}

/* ---- The new map ---- */

typedef struct {
    uint32_t sector;                    /* bytes */
    uint32_t idlen, log2bpmb, zones;
    uint32_t zone_bits;                 /* a zone's bits for the disc */
    uint64_t bits;                      /* the disc's */
    uint64_t size;                      /* bytes */
    uint64_t map;                       /* the map's first copy */
} layout_t;

/* The map's layout from a disc record; false if it makes no sense */
static bool layout(const uint8_t *dr, layout_t *l)
{
    uint32_t log2sec = dr[0], spare = dr[10] | dr[11] << 8;
    l->idlen = dr[4];
    l->log2bpmb = dr[5];
    l->zones = dr[9] | dr[42] << 8;     /* nzones, nzones_high */
    l->size = le32(dr + 16) | (uint64_t)le32(dr + 36) << 32;
    if (log2sec < 8 || log2sec > LOG2_SECTOR_MAX || l->idlen < 8 || l->idlen > 24 || l->log2bpmb > 16 ||
        l->zones == 0 || l->size == 0) {
        return false;
    }
    l->sector = 1u << log2sec;
    if (spare < 32 || spare + DR_BITS >= 8 * l->sector) {
        return false;
    }
    l->zone_bits = 8 * l->sector - spare;
    l->bits = l->size >> l->log2bpmb;
    /* the zones cover the disc, the last one some of it */
    uint64_t last = (uint64_t)(l->zones - 1) * l->zone_bits;
    if (l->bits + DR_BITS > last + l->zone_bits || l->bits + DR_BITS <= last) {
        return false;
    }
    l->map = l->zones == 1 ? 0 : ((uint64_t)(l->zones / 2) * l->zone_bits - DR_BITS) << l->log2bpmb;
    l->map &= ~(uint64_t)(l->sector - 1);
    return true;
}

static bool bit(const uint8_t *z, uint32_t n)
{
    return z[n >> 3] >> (n & 7) & 1;
}

static uint32_t bits(const uint8_t *z, uint32_t n, uint32_t count)
{
    uint32_t v = 0;
    while (count-- > 0) {
        v = v << 1 | bit(z, n + count);
    }
    return v;
}

/* A zone's check byte, as FileCore works it out (byte 0 left out) */
static uint8_t zone_check(const uint8_t *z, uint32_t size)
{
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (uint32_t i = size - 4; i > 0; i -= 4) {
        s0 += z[i] + (s3 >> 8);
        s3 &= 0xFF;
        s1 += z[i + 1] + (s0 >> 8);
        s0 &= 0xFF;
        s2 += z[i + 2] + (s1 >> 8);
        s1 &= 0xFF;
        s3 += z[i + 3] + (s2 >> 8);
        s2 &= 0xFF;
    }
    s0 += s3 >> 8;
    s1 += z[1] + (s0 >> 8);
    s2 += z[2] + (s1 >> 8);
    s3 += z[3] + (s2 >> 8);
    return (uint8_t)(s0 ^ s1 ^ s2 ^ s3);
}

/* A zone's free bits, -1 if its free list is broken. Fragments lie from
 * first to end (its bits for the disc); those from limit on are past the
 * disc's end (the last zone's).
 */
static int32_t zone_free(const uint8_t *z, uint32_t first, uint32_t end, uint32_t limit, uint32_t idlen)
{
    uint32_t link = idlen < 15 ? idlen : 15;
    uint32_t at = bits(z, 8, 15);
    int32_t n = 0;
    if (at == 0) {
        return 0;
    }
    at += 8;
    for (;;) {
        if (at < first || at + idlen >= end) {
            return -1;
        }
        uint32_t next = bits(z, at, link), stop = at + idlen;
        while (stop < end && !bit(z, stop)) {
            stop++;                     /* to the fragment's last bit */
        }
        if (stop == end) {
            return -1;
        }
        if (at < limit) {
            n += (int32_t)((stop < limit ? stop + 1 : limit) - at);
        }
        if (next == 0) {
            return n;
        }
        if (at + next <= stop) {
            return -1;
        }
        at += next;
    }
}

/* The free bits of the map's copy at map: false if a zone's check byte,
 * the cross check or a free list is wrong
 */
static bool count_map(const diskspace_disk_t *d, const layout_t *l, uint64_t map, uint64_t *free_bits)
{
    uint8_t z[1u << LOG2_SECTOR_MAX];
    uint8_t cross = 0;
    uint64_t n = 0;
    for (uint32_t i = 0; i < l->zones; i++) {
        if (!d->read(d->ctx, map + (uint64_t)i * l->sector, z, l->sector) || zone_check(z, l->sector) != z[0]) {
            return false;
        }
        cross ^= z[3];
        uint32_t first = i == 0 ? 32 + DR_BITS : 32, end = 32 + l->zone_bits, limit = end;
        if (i == l->zones - 1) {
            limit = (uint32_t)(32 + DR_BITS + l->bits - (uint64_t)i * l->zone_bits);
        }
        int32_t f = zone_free(z, first, end, limit, l->idlen);
        if (f < 0) {
            return false;
        }
        n += (uint64_t)f;
    }
    *free_bits = n;
    return cross == 0xFF;
}

/* The disc's free space from its map's first copy, else the second */
static bool new_map(const diskspace_disk_t *d, const layout_t *l, diskspace_t *out)
{
    uint64_t f;
    for (int copy = 0; copy < 2; copy++) {
        if (count_map(d, l, l->map + (uint64_t)copy * l->zones * l->sector, &f)) {
            out->free = f << l->log2bpmb;
            out->size = l->size;
            return out->free <= out->size;
        }
    }
    return false;
}

bool diskspace_adfs(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t b[512];
    layout_t l;
    if (old_map(d, out)) {
        return true;
    }
    /* hard discs, F floppies: the boot block's disc record */
    if (d->read(d->ctx, BOOT_BLOCK, b, sizeof(b)) && sum_check(b, 511, 0) == b[511] && layout(b + 0x1C0, &l) &&
        new_map(d, &l, out)) {
        return true;
    }
    /* E floppies (one zone, at the start), an F floppy without a boot block:
     * zone 0's own copy of the disc record, where it says the map is
     */
    static const uint32_t zone0[] = { 0, F_MAP };
    for (size_t i = 0; i < sizeof(zone0) / sizeof(zone0[0]); i++) {
        if (d->read(d->ctx, zone0[i] + 4, b, 60) && layout(b, &l) && l.map == zone0[i] && new_map(d, &l, out)) {
            return true;
        }
    }
    return false;
}
