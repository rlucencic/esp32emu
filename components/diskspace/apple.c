/*
 * Apple's filesystems: MFS, HFS and HFS+ (the Macintosh, GS/OS), Apple
 * partition maps, ProDOS and DOS 3.3 (the Apple II).
 *
 * MFS and HFS keep their counts in the master directory block, 1024 bytes
 * into the volume: allocation blocks, their size, how many are free. HFS+
 * has its volume header there instead, or, wrapped in HFS, in the embedded
 * volume the wrapper's MDB points at. ProDOS and DOS 3.3 count nothing:
 * their bitmaps have a bit a block or sector, set when it's free.
 */
#include <string.h>
#include "diskspace.h"

#define SIG_MFS         0xD2D7
#define SIG_HFS         0x4244      /* "BD" */
#define SIG_HFS_PLUS    0x482B      /* "H+" */
#define SIG_HFSX        0x4858      /* "HX" */
#define BLOCK           512
#define MAP_ENTRIES     64          /* partition map entries looked at */
#define VTOC            (17 * 16 * 256)     /* DOS 3.3: track 17, sector 0 */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* size bytes from base lie within the image (when its size is known) */
static bool fits(const diskspace_disk_t *d, uint64_t base, uint64_t size)
{
    return d->size == 0 || (base <= d->size && size <= d->size - base);
}

/* The set bits among the first n of a bitmap, highest bit of each byte
 * first
 */
static uint32_t bits_set(const uint8_t *p, uint32_t n)
{
    uint32_t count = 0;
    for (; n >= 8; n -= 8) {
        count += (uint32_t)__builtin_popcount(*p++);
    }
    if (n > 0) {
        count += (uint32_t)__builtin_popcount(*p & (0xFF00u >> n) & 0xFF);
    }
    return count;
}

/* HFS+ and HFSX: the volume header's block size, total and free blocks */
static bool hfs_plus(const diskspace_disk_t *d, uint64_t base, diskspace_t *out)
{
    uint8_t h[52];
    if (!d->read(d->ctx, base + 1024, h, sizeof(h)) || (be16(h) != SIG_HFS_PLUS && be16(h) != SIG_HFSX)) {
        return false;
    }
    uint32_t block = be32(h + 40), total = be32(h + 44), free_blocks = be32(h + 48);
    uint64_t size = (uint64_t)total * block;
    if (block == 0 || block % BLOCK != 0 || total == 0 || free_blocks > total || !fits(d, base, size)) {
        return false;
    }
    out->free = (uint64_t)free_blocks * block;
    out->size = size;
    return true;
}

bool diskspace_hfs(const diskspace_disk_t *d, uint64_t base, diskspace_t *out)
{
    uint8_t m[130];
    if (!d->read(d->ctx, base + 1024, m, sizeof(m))) {
        return false;
    }
    uint16_t sig = be16(m);
    if (sig == SIG_HFS_PLUS || sig == SIG_HFSX) {
        return hfs_plus(d, base, out);
    }
    if (sig != SIG_MFS && sig != SIG_HFS) {
        return false;
    }
    /* drNmAlBlks, drAlBlkSiz, drFreeBks; the allocation blocks start at
     * drAlBlSt (512-byte sectors)
     */
    uint32_t blocks = be16(m + 18), block = be32(m + 20), free_blocks = be16(m + 34);
    uint64_t first = (uint64_t)be16(m + 28) * BLOCK;
    if (block == 0 || block % BLOCK != 0 || blocks == 0 || free_blocks > blocks ||
        !fits(d, base, first + (uint64_t)blocks * block)) {
        return false;
    }
    /* An HFS wrapper: the HFS+ volume in it (drEmbedExtent's first block) */
    if (sig == SIG_HFS && be16(m + 124) == SIG_HFS_PLUS) {
        return hfs_plus(d, base + first + (uint64_t)be16(m + 126) * block, out);
    }
    out->free = (uint64_t)free_blocks * block;
    out->size = (uint64_t)blocks * block;
    return true;
}

/* Block 0 "ER" (the driver descriptor), then the map's entries "PM" from
 * block 1, each with the map's length, its start and length in blocks and
 * its type
 */
bool diskspace_mac_disk(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t b[BLOCK];
    if (!d->read(d->ctx, 0, b, 2)) {
        return false;
    }
    if (b[0] != 'E' || b[1] != 'R') {
        return diskspace_hfs(d, 0, out);
    }
    bool any = false;
    uint64_t free_bytes = 0, size = 0;
    uint32_t entries = 1;
    for (uint32_t i = 1; i <= entries && i <= MAP_ENTRIES; i++) {
        if (!d->read(d->ctx, (uint64_t)i * BLOCK, b, sizeof(b)) || b[0] != 'P' || b[1] != 'M') {
            break;
        }
        entries = be32(b + 4);
        diskspace_t v;
        if (strncmp((const char *)b + 48, "Apple_HFS", 9) == 0 &&
            diskspace_hfs(d, (uint64_t)be32(b + 8) * BLOCK, &v)) {
            free_bytes += v.free;
            size += v.size;
            any = true;
        }
    }
    if (any) {
        out->free = free_bytes;
        out->size = size;
    }
    return any;
}

/* The volume directory's key block: its header (storage type $F and a
 * name, ProDOS's entry length and entries a block), the bitmap's first
 * block and the volume's blocks
 */
bool diskspace_prodos(const diskspace_disk_t *d, uint64_t base, diskspace_t *out)
{
    uint8_t b[BLOCK];
    if (!d->read(d->ctx, base + 2 * BLOCK, b, sizeof(b))) {
        return false;
    }
    uint32_t bitmap = le16(b + 0x27), total = le16(b + 0x29);
    uint32_t bitmap_blocks = (total + BLOCK * 8 - 1) / (BLOCK * 8);
    if (le16(b) != 0 || b[4] >> 4 != 0xF || (b[4] & 0xF) == 0 || b[0x23] != 0x27 || b[0x24] != 0x0D ||
        bitmap <= 2 || bitmap + bitmap_blocks > total || !fits(d, base, (uint64_t)total * BLOCK)) {
        return false;
    }
    uint32_t free_blocks = 0;
    for (uint32_t i = 0, done = 0; i < bitmap_blocks; i++, done += BLOCK * 8) {
        if (!d->read(d->ctx, base + (uint64_t)(bitmap + i) * BLOCK, b, sizeof(b))) {
            return false;
        }
        uint32_t n = total - done < BLOCK * 8 ? total - done : BLOCK * 8;
        free_blocks += bits_set(b, n);
    }
    out->free = (uint64_t)free_blocks * BLOCK;
    out->size = (uint64_t)total * BLOCK;
    return true;
}

/* The VTOC: catalog track, DOS release, tracks, sectors a track, bytes a
 * sector, then 4 bytes a track of which the first two are its sectors
 * (15 .. 0)
 */
bool diskspace_dos33(const diskspace_disk_t *d, diskspace_t *out)
{
    uint8_t v[256];
    if (!d->read(d->ctx, VTOC, v, sizeof(v))) {
        return false;
    }
    uint32_t tracks = v[0x34], sectors = v[0x35];
    uint64_t size = (uint64_t)tracks * sectors * 256;
    if (v[0x03] != 3 || v[0x01] == 0 || v[0x01] >= tracks || tracks < 35 || tracks > 50 || sectors != 16 ||
        le16(v + 0x36) != 256 || !fits(d, 0, size)) {
        return false;
    }
    uint32_t free_sectors = 0;
    for (uint32_t t = 0; t < tracks; t++) {
        free_sectors += bits_set(v + 0x38 + 4 * t, 16);
    }
    out->free = (uint64_t)free_sectors * 256;
    out->size = size;
    return true;
}
