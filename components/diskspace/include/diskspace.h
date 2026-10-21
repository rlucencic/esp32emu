/*
 * Free space on disk images, as their own filesystems count it: the
 * machines show it on the web page under each drive
 * (emu_json_drive_space()).
 *
 * Each counter reads the image through the machine's callback, by byte
 * offset in the image's (or the volume's) logical order, so an image in
 * PSRAM, in a file or kept as tracks works alike. A counter answers only
 * when its filesystem is there and its figures are sane (false otherwise:
 * try another, or show nothing). They read little (a header and an
 * allocation map; a big FAT's sectors at most) and keep nothing between
 * calls. The machine calls them where the image is safe to read (the task
 * that owns it, or under emu_paused()), at insert and after its writes are
 * flushed, and keeps the answer for the web page.
 *
 * Platform independent; host tests in test/ (make -C test).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* len bytes at offset into buf; false if it can't (past the end) */
    bool (*read)(void *ctx, uint64_t offset, void *buf, uint32_t len);
    void *ctx;
    uint64_t size;                  /* the image's bytes (0: not known) */
} diskspace_disk_t;

typedef struct {
    uint64_t free;                  /* bytes free for files */
    uint64_t size;                  /* the volume's bytes */
} diskspace_t;

/* ---- fat.c: MS-DOS, Atari ST, MSX, PC, Archimedes DOS discs ---- */

/* A FAT12/16/32 volume whose boot sector is at base (FAT32: FSInfo's count
 * when it's valid). Atari ST boot sectors (no 0x55AA, often no jump) too.
 */
bool diskspace_fat(const diskspace_disk_t *d, uint64_t base, diskspace_t *out);
/* A hard disk: its MBR's FAT partitions added up, or one FAT volume from
 * sector 0 (a floppy, a "superfloppy")
 */
bool diskspace_fat_disk(const diskspace_disk_t *d, diskspace_t *out);

/* ---- apple.c: Macintosh, Apple IIGS ---- */

/* MFS, HFS or HFS+ with its volume header at base + 1024 */
bool diskspace_hfs(const diskspace_disk_t *d, uint64_t base, diskspace_t *out);
/* A Mac disk: an Apple partition map's HFS partitions added up, or one
 * volume from 0
 */
bool diskspace_mac_disk(const diskspace_disk_t *d, diskspace_t *out);
/* ProDOS: the volume directory in block 2 (512-byte blocks from base), its
 * bitmap
 */
bool diskspace_prodos(const diskspace_disk_t *d, uint64_t base, diskspace_t *out);
/* DOS 3.3 (140 KB, 256-byte sectors in DOS order): the VTOC's bitmaps */
bool diskspace_dos33(const diskspace_disk_t *d, diskspace_t *out);

/* ---- cpm.c: CP/M 2.2 and 3 (Amstrad CPC's AMSDOS, the Spectrum +3's
 * +3DOS) ---- */

typedef struct {
    uint32_t sector_size;           /* bytes */
    uint32_t sectors;               /* a track's */
    uint32_t tracks;                /* the disk's, both sides counted */
    uint32_t reserved_tracks;       /* before the directory */
    uint32_t block_size;            /* 1024, 2048, ... */
    uint32_t dir_entries;           /* 32-byte entries */
} diskspace_cpm_geometry_t;

/* The blocks no directory entry uses. Offsets: track t's sector s (its
 * place in the track, from 0) at (t * sectors + s) * sector_size; the
 * machine maps them onto its image (sector IDs, sides).
 */
bool diskspace_cpm(const diskspace_disk_t *d, const diskspace_cpm_geometry_t *g, diskspace_t *out);

/* ---- trdos.c: the Spectrum's Beta Disk ---- */

/* TR-DOS: the system sector's count (track 0, sector 9; 256-byte sectors,
 * 16 a track, linear)
 */
bool diskspace_trdos(const diskspace_disk_t *d, diskspace_t *out);

/* ---- cbm.c: Commodore 1541, 1571, 1581 ---- */

/* D64, D71, D81 (by the image's size): the BAM's free blocks, the
 * directory track left out as the drive does ("blocks free")
 */
bool diskspace_cbm(const diskspace_disk_t *d, diskspace_t *out);

/* ---- atari.c: Atari 8-bit ---- */

/* Atari DOS 2.0/2.5 and MyDOS (the VTOC in sector 360; DOS 2.5's second
 * in 1024), SpartaDOS. Sectors from 1 of sector_size 128 or 256 (a 256-byte
 * disk's first three are 128), offsets after an ATR's 16-byte header.
 */
bool diskspace_atari(const diskspace_disk_t *d, uint32_t sector_size, diskspace_t *out);

/* ---- coco.c: Tandy Color Computer ---- */

/* Disk BASIC (RS-DOS): track 17's granule table (256-byte sectors, 18 a
 * track, linear)
 */
bool diskspace_rsdos(const diskspace_disk_t *d, diskspace_t *out);
/* OS-9 / NitrOS-9: LSN0's figures, the allocation bitmap from LSN1
 * (256-byte sectors, linear)
 */
bool diskspace_os9(const diskspace_disk_t *d, diskspace_t *out);

/* ---- acorn.c: Acorn Archimedes ---- */

/* ADFS / FileCore: the old map (S, M, L floppies) or the new map (D, E, F
 * floppies, hard discs: the disc record in the boot block at 0xC00 for
 * hard discs)
 */
bool diskspace_adfs(const diskspace_disk_t *d, diskspace_t *out);
