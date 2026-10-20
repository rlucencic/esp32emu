/*
 * uPD765A floppy disk controller with DSK and extended DSK images, shared
 * by the Amstrad CPC (../esp32cpc) and the Spectrum +3 (../esp32zx), which
 * wire it up the same way: non-DMA, no terminal count, two drives. The IBM
 * PC (../esp32pc) wires it otherwise (upd765_wire(): DMA with terminal
 * count, the interrupt line, both unit select lines, multi-track
 * operation) and attaches raw sector images it keeps itself
 * (upd765_disk_attach()).
 *
 * The machine decodes its ports and calls upd765_status() (main status
 * register), upd765_read() / upd765_write() (data register) and
 * upd765_motor(); upd765_frame() once per video frame (the drive light).
 *
 * Images are read into memory; writes go back to the file with
 * upd765_disk_flush() (only the tracks that changed, unless a format
 * changed the layout: then the whole file, as EDSK). Disk calls not while
 * the machine runs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UPD765_DRIVES   2

void upd765_reset(void);
uint8_t upd765_status(void);
uint8_t upd765_read(void);
void upd765_write(uint8_t v);
void upd765_motor(bool on);
void upd765_frame(void);

bool upd765_disk_insert(int drive, const char *path);
void upd765_disk_eject(int drive);
const char *upd765_disk_path(int drive);        /* NULL when empty */
bool upd765_disk_write_protected(int drive);
bool upd765_disk_dirty(void);                   /* something to flush */
void upd765_disk_flush(void);

/* A formatted blank disk: single-sided, `cyls` tracks of `sectors` 512-byte
 * sectors numbered from first_id, filled with $E5; first_sector (512
 * bytes, NULL: none) goes into track 0's first sector
 */
bool upd765_disk_create(const char *path, int cyls, int sectors, uint8_t first_id,
                        const uint8_t *first_sector);

/* The "creator" field of EDSK files written ("esp32emu" by default) */
void upd765_set_creator(const char *name);

/* The PC's wiring; all of it optional, zeros keep the CPC's */
typedef struct {
    /* DMA (unless SPECIFY chose non-DMA): a sector's bytes to memory or from
     * it; returns how many moved, *tc when the DMA's count ran out (terminal
     * count: the command ends after this sector, normally)
     */
    int (*dma)(bool to_memory, uint8_t *buf, int len, bool *tc);
    void (*irq)(bool level);        /* the INT line: results, seeks, a reset */
    void (*seek)(int drive);        /* a step pulse (the PC's disk change line clears) */
    bool four_units;                /* both unit selects decoded: drives 2, 3 never ready */
    bool multitrack;                /* MT: a read or write goes on to head 1 */
} upd765_wiring_t;

void upd765_wire(const upd765_wiring_t *wiring);

/* A raw image the machine keeps: cylinders x heads x sectors of 512 bytes,
 * sectors numbered from 1 on each track, read and written through `io`
 * (a format that keeps the layout writes its filler through it)
 */
typedef struct {
    bool (*read)(int drive, int c, int h, int r, uint8_t *buf);
    bool (*write)(int drive, int c, int h, int r, const uint8_t *buf);
} upd765_raw_t;

bool upd765_disk_attach(int drive, const upd765_raw_t *io, int cyls, int heads, int sectors, bool wp,
                        const char *path);

typedef struct {
    int light;                      /* drive accessed in the last ~0.3 s, -1 none */
    uint32_t reads[UPD765_DRIVES];  /* sectors read and written */
    uint32_t writes[UPD765_DRIVES];
    uint8_t track[UPD765_DRIVES];   /* head position */
    bool motor;
} upd765_status_t;

void upd765_disk_status(upd765_status_t *status);
