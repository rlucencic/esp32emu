/*
 * uPD765A floppy controller and the disk images: see upd765.h.
 *
 * Non-DMA mode as on the CPC and the Spectrum +3: the CPU moves every byte
 * through the data register. Data is ready at once (no rotation timing);
 * reads end the way AMSDOS and +3DOS expect, with the terminal count line
 * not connected: after the last sector with "end of cylinder".
 *
 * Images: "MV - CPC" (standard DSK) and "EXTENDED CPC DSK File" (EDSK:
 * sizes per track and sector, sector status bytes, several copies of a
 * weak sector). Read into memory; writes go back with upd765_disk_flush(),
 * track by track in place, or the whole file (as EDSK) when formatting
 * changed its layout. Raw images the machine keeps (the PC's): the tracks'
 * IDs here, each sector through its callbacks.
 *
 * The PC's wiring (upd765_wire()): DMA moves each sector at once, the
 * terminal count ends a command normally after its sector; the INT line
 * at a data command's result (until its first byte is read), at a seek's
 * end and after a reset (falling at each SENSE INTERRUPT STATUS, which
 * tells the units' statuses one by one; an empty drive is ready);
 * multi-track reads and writes go on to head 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upd765.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define DSK_LOG(fmt, ...)   ESP_LOGW("upd765", fmt, ##__VA_ARGS__)
/* Tracks in PSRAM: plain malloc() serves small blocks (a track is ~4.6 KB)
 * from internal RAM, and a disk image used it up
 */
#define dsk_malloc(n)       heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define dsk_calloc(n, s)    heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#else
#define DSK_LOG(fmt, ...)   fprintf(stderr, "upd765: " fmt "\n", ##__VA_ARGS__)
#define dsk_malloc(n)       malloc(n)
#define dsk_calloc(n, s)    calloc((n), (s))
#endif

#define MAX_CYL         84
#define MAX_SECTORS     29

typedef struct {
    uint8_t c, h, r, n, st1, st2;
    uint16_t len;               /* bytes stored */
    uint32_t off;               /* in the track's data */
} sector_t;

typedef struct {
    bool present;
    uint8_t nsec, n, gap3, filler;
    sector_t sec[MAX_SECTORS];
    uint8_t *data;
    uint32_t size;              /* data bytes */
    uint32_t file_off;          /* its Track-Info block in the file, 0: not in the file */
    uint32_t file_size;         /* block size in the file (header included) */
    bool dirty;
} track_t;

typedef struct {
    char path[256];
    bool wp;
    uint8_t cyls, sides;
    bool relayout;              /* the file must be written anew */
    track_t trk[MAX_CYL][2];
    uint8_t id_index;
    uint32_t weak;
    const upd765_raw_t *raw;    /* the machine's raw image (tracks without data here) */
} disk_t;

#define UNITS           4       /* unit numbers in commands (the CPC's: 0, 1) */

static disk_t *s_disk[UPD765_DRIVES];
static upd765_wiring_t w;
static uint8_t s_sector[1024];  /* a raw image's sector in transfer */
static char s_creator[15] = "esp32emu";
static upd765_status_t s_stat;
static int s_light_frames;

enum { PH_IDLE, PH_CMD, PH_EXEC_READ, PH_EXEC_WRITE, PH_RESULT };

static struct {
    int phase;
    uint8_t msr;
    uint8_t cmd[9];
    int cmd_len, cmd_pos;
    uint8_t res[7];
    int res_len, res_pos;
    uint8_t pcn[UNITS];
    uint8_t seek_st0[UNITS];
    bool seek_pending[UNITS];
    bool reset_pending[UNITS];  /* the PC's: ready changed, after a reset */
    bool motor;
    bool nd;                    /* SPECIFY: non-DMA */
    bool int_line;

    /* The command running */
    uint8_t op;
    int drive, side;
    uint8_t c, h, r, n, eot, dtl;
    bool mt, sk;
    uint8_t st0, st1, st2;
    track_t *trk;
    sector_t *sec;
    uint8_t *xfer;
    int xfer_len, xfer_pos;
    int track_index;            /* READ TRACK: sectors done */
    uint8_t fmt[4 * MAX_SECTORS];
    int fmt_len;
} f;

/* ---- Images ---- */

static void free_disk(disk_t *d)
{
    if (d == NULL) {
        return;
    }
    for (int c = 0; c < MAX_CYL; c++) {
        for (int s = 0; s < 2; s++) {
            free(d->trk[c][s].data);
        }
    }
    free(d);
}

static uint16_t sector_size(uint8_t n)
{
    return n > 6 ? 0x1800 : 128u << n;
}

/* One Track-Info block at `p` (block_size bytes) into t */
static bool parse_track(track_t *t, const uint8_t *p, uint32_t block_size, bool extended)
{
    if (block_size < 256 || memcmp(p, "Track-Info", 10) != 0) {
        return false;
    }
    t->present = true;
    t->n = p[0x14];
    t->nsec = p[0x15] > MAX_SECTORS ? MAX_SECTORS : p[0x15];
    t->gap3 = p[0x16];
    t->filler = p[0x17];
    uint32_t off = 0;
    for (int i = 0; i < t->nsec; i++) {
        const uint8_t *s = p + 0x18 + i * 8;
        sector_t *sec = &t->sec[i];
        sec->c = s[0];
        sec->h = s[1];
        sec->r = s[2];
        sec->n = s[3];
        sec->st1 = s[4];
        sec->st2 = s[5];
        uint16_t len = extended ? (s[6] | (s[7] << 8)) : sector_size(t->n);
        if (extended && len == 0) {
            len = sector_size(sec->n);
        }
        sec->len = len;
        sec->off = off;
        off += len;
    }
    if (256 + off > block_size) {
        off = block_size - 256;     /* a short image: what's there */
    }
    t->size = off;
    t->data = dsk_malloc(off ? off : 1);
    if (t->data == NULL) {
        return false;
    }
    memcpy(t->data, p + 256, off);
    for (int i = 0; i < t->nsec; i++) {
        if (t->sec[i].off + t->sec[i].len > off) {
            t->sec[i].len = t->sec[i].off < off ? off - t->sec[i].off : 0;
        }
    }
    return true;
}

static disk_t *load_image(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *file = (size >= 256 && size < 4 * 1024 * 1024) ? dsk_malloc(size) : NULL;
    if (file == NULL || fread(file, 1, size, fp) != (size_t)size) {
        free(file);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    bool extended = memcmp(file, "EXTENDED", 8) == 0;
    bool standard = memcmp(file, "MV - CPC", 8) == 0;
    disk_t *d = (extended || standard) ? dsk_calloc(1, sizeof(disk_t)) : NULL;
    if (d == NULL) {
        free(file);
        return NULL;
    }
    d->cyls = file[0x30] > MAX_CYL ? MAX_CYL : file[0x30];
    d->sides = file[0x31] == 2 ? 2 : 1;
    uint32_t pos = 256;
    bool ok = true;
    for (int c = 0; c < d->cyls && ok; c++) {
        for (int s = 0; s < d->sides && ok; s++) {
            uint32_t block = extended ? file[0x34 + c * d->sides + s] * 256u
                                      : (uint32_t)(file[0x32] | (file[0x33] << 8));
            if (block == 0) {
                continue;       /* unformatted track */
            }
            if (pos >= (uint32_t)size) {
                break;
            }
            uint32_t avail = (uint32_t)size - pos < block ? (uint32_t)size - pos : block;
            track_t *t = &d->trk[c][s];
            if (parse_track(t, file + pos, avail, extended)) {
                t->file_off = pos;
                t->file_size = block;
            } else if (memcmp(file + pos, "Track-Info", 10) == 0) {
                ok = false;     /* out of memory */
            }
            pos += block;
        }
    }
    free(file);
    if (!ok) {
        free_disk(d);
        return NULL;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    return d;
}

/* The whole image as EDSK */
static bool write_image(disk_t *d)
{
    FILE *fp = fopen(d->path, "wb");
    if (fp == NULL) {
        return false;
    }
    uint8_t h[256] = { 0 };
    memcpy(h, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
    memcpy(h + 0x22, s_creator, strlen(s_creator));
    h[0x30] = d->cyls;
    h[0x31] = d->sides;
    for (int c = 0; c < d->cyls; c++) {
        for (int s = 0; s < d->sides; s++) {
            track_t *t = &d->trk[c][s];
            h[0x34 + c * d->sides + s] = t->present ? (256 + t->size + 255) / 256 : 0;
        }
    }
    bool ok = fwrite(h, 1, 256, fp) == 256;
    uint32_t pos = 256;
    for (int c = 0; c < d->cyls && ok; c++) {
        for (int s = 0; s < d->sides && ok; s++) {
            track_t *t = &d->trk[c][s];
            if (!t->present) {
                continue;
            }
            uint8_t th[256] = { 0 };
            memcpy(th, "Track-Info\r\n", 12);
            th[0x10] = c;
            th[0x11] = s;
            th[0x14] = t->n;
            th[0x15] = t->nsec;
            th[0x16] = t->gap3;
            th[0x17] = t->filler;
            for (int i = 0; i < t->nsec; i++) {
                uint8_t *p = th + 0x18 + i * 8;
                sector_t *sec = &t->sec[i];
                p[0] = sec->c;
                p[1] = sec->h;
                p[2] = sec->r;
                p[3] = sec->n;
                p[4] = sec->st1;
                p[5] = sec->st2;
                p[6] = sec->len;
                p[7] = sec->len >> 8;
            }
            uint32_t block = (256 + t->size + 255) & ~255u;
            static const uint8_t zeros[256];
            ok = fwrite(th, 1, 256, fp) == 256 && fwrite(t->data, 1, t->size, fp) == t->size &&
                 fwrite(zeros, 1, block - 256 - t->size, fp) == block - 256 - t->size;
            t->file_off = pos;
            t->file_size = block;
            t->dirty = false;
            pos += block;
        }
    }
    fclose(fp);
    if (ok) {
        d->relayout = false;
    }
    return ok;
}

/* Changed tracks in place */
static bool write_tracks(disk_t *d)
{
    FILE *fp = fopen(d->path, "r+b");
    if (fp == NULL) {
        return false;
    }
    bool ok = true;
    for (int c = 0; c < d->cyls && ok; c++) {
        for (int s = 0; s < d->sides && ok; s++) {
            track_t *t = &d->trk[c][s];
            if (t->dirty) {
                ok = fseek(fp, t->file_off + 256, SEEK_SET) == 0 &&
                     fwrite(t->data, 1, t->size, fp) == t->size;
                t->dirty = false;
            }
        }
    }
    fclose(fp);
    return ok;
}

bool upd765_disk_dirty(void)
{
    for (int dr = 0; dr < UPD765_DRIVES; dr++) {
        disk_t *d = s_disk[dr];
        if (d == NULL || d->raw) {
            continue;
        }
        if (d->relayout) {
            return true;
        }
        for (int c = 0; c < d->cyls; c++) {
            if (d->trk[c][0].dirty || d->trk[c][1].dirty) {
                return true;
            }
        }
    }
    return false;
}

void upd765_disk_flush(void)
{
    for (int dr = 0; dr < UPD765_DRIVES; dr++) {
        disk_t *d = s_disk[dr];
        if (d == NULL || d->wp || d->raw) {
            continue;
        }
        bool ok = d->relayout ? write_image(d) : write_tracks(d);
        if (!ok) {
            DSK_LOG("can't write %s", d->path);
        }
    }
}

bool upd765_disk_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= UPD765_DRIVES) {
        return false;
    }
    disk_t *d = load_image(path);
    if (d == NULL) {
        return false;
    }
    FILE *fp = fopen(path, "r+b");
    d->wp = fp == NULL;
    if (fp != NULL) {
        fclose(fp);
    }
    upd765_disk_eject(drive);
    s_disk[drive] = d;
    return true;
}

void upd765_disk_eject(int drive)
{
    if (drive < 0 || drive >= UPD765_DRIVES || s_disk[drive] == NULL) {
        return;
    }
    if (upd765_disk_dirty()) {
        upd765_disk_flush();
    }
    free_disk(s_disk[drive]);
    s_disk[drive] = NULL;
}

const char *upd765_disk_path(int drive)
{
    return drive >= 0 && drive < UPD765_DRIVES && s_disk[drive] ? s_disk[drive]->path : NULL;
}

bool upd765_disk_write_protected(int drive)
{
    return drive >= 0 && drive < UPD765_DRIVES && s_disk[drive] && s_disk[drive]->wp;
}

bool upd765_disk_create(const char *path, int cyls, int sectors, uint8_t first_id,
                        const uint8_t *first_sector)
{
    if (cyls < 1 || cyls > MAX_CYL || sectors < 1 || sectors > MAX_SECTORS) {
        return false;
    }
    disk_t *d = dsk_calloc(1, sizeof(disk_t));
    if (d == NULL) {
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->cyls = cyls;
    d->sides = 1;
    bool ok = true;
    for (int c = 0; c < cyls && ok; c++) {
        track_t *t = &d->trk[c][0];
        t->present = true;
        t->n = 2;
        t->nsec = sectors;
        t->gap3 = 0x4E;
        t->filler = 0xE5;
        t->size = sectors * 512;
        t->data = dsk_malloc(t->size);
        ok = t->data != NULL;
        if (ok) {
            memset(t->data, 0xE5, t->size);
            if (c == 0 && first_sector) {
                memcpy(t->data, first_sector, 512);
            }
        }
        for (int i = 0; i < sectors; i++) {
            t->sec[i] = (sector_t){ .c = c, .h = 0, .r = first_id + i, .n = 2, .len = 512, .off = i * 512 };
        }
    }
    ok = ok && write_image(d);
    free_disk(d);
    return ok;
}

void upd765_set_creator(const char *name)
{
    snprintf(s_creator, sizeof(s_creator), "%s", name);
}

void upd765_disk_status(upd765_status_t *s)
{
    *s = s_stat;
    s->motor = f.motor;
    for (int d = 0; d < UPD765_DRIVES; d++) {
        s->track[d] = f.pcn[d];
    }
}

/* ---- The controller ---- */

static void set_int(bool on);

static int command_length(uint8_t op)
{
    switch (op) {
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C: case 0x11: case 0x19: case 0x1D:
        return 9;
    case 0x03: case 0x0F:
        return 3;
    case 0x04: case 0x07: case 0x0A:
        return 2;
    case 0x0D:
        return 6;
    default:
        return 1;
    }
}

static void result(int len)
{
    f.phase = PH_RESULT;
    f.res_len = len;
    f.res_pos = 0;
    f.msr = 0xD0;
}

static void idle(void)
{
    f.phase = PH_IDLE;
    f.cmd_pos = 0;
    f.msr = 0x80;
}

static uint8_t st0_base(void)
{
    return (f.side << 2) | f.drive;
}

/* Read/write/format results: ST0 ST1 ST2 C H R N */
static void rw_result(void)
{
    f.res[0] = f.st0;
    f.res[1] = f.st1;
    f.res[2] = f.st2;
    f.res[3] = f.c;
    f.res[4] = f.h;
    f.res[5] = f.r;
    f.res[6] = f.n;
    result(7);
    set_int(true);
}

static void rw_error(uint8_t st0, uint8_t st1, uint8_t st2)
{
    f.st0 = st0_base() | st0;
    f.st1 = st1;
    f.st2 = st2;
    rw_result();
}

static disk_t *disk(void)
{
    return f.drive < UPD765_DRIVES ? s_disk[f.drive] : NULL;
}

static void set_int(bool on)
{
    if (w.irq && f.int_line != on) {
        f.int_line = on;
        w.irq(on);
    }
}

static bool dma_on(void)
{
    return w.dma && !f.nd;
}

static void light(void)
{
    s_stat.light = f.drive;
    s_light_frames = 15;
}

static track_t *current_track(void)
{
    disk_t *d = disk();
    if (d == NULL || f.pcn[f.drive] >= d->cyls || f.side >= d->sides) {
        return NULL;
    }
    track_t *t = &d->trk[f.pcn[f.drive]][f.side];
    return t->present ? t : NULL;
}

static sector_t *find_sector(track_t *t)
{
    for (int i = 0; i < t->nsec; i++) {
        sector_t *s = &t->sec[i];
        if (s->c == f.c && s->h == f.h && s->r == f.r && s->n == f.n) {
            return s;
        }
    }
    return NULL;
}

/* Bytes of a sector for this transfer (a weak sector: one of its copies) */
static void sector_window(sector_t *s, uint8_t **p, int *len)
{
    int nominal = f.n ? sector_size(f.n) : f.dtl;
    *p = f.trk->data + s->off;
    *len = s->len;
    if (nominal && s->len > nominal && s->len % nominal == 0) {
        int copies = s->len / nominal;
        *p += (disk()->weak++ % copies) * nominal;
        *len = nominal;
    } else if (f.n == 0 && f.dtl && f.dtl < s->len) {
        *len = f.dtl;
    }
}

static void end_of_cylinder(void)
{
    f.st0 = st0_base() | 0x40;
    f.st1 |= 0x80;
    f.c++;
    f.r = 1;
    if (w.multitrack && f.mt) {
        f.h ^= 1;
    }
    rw_result();
}

/* The terminal count after a sector: a normal end, the next sector's ID
 * in the result (the datasheet's table)
 */
static void terminal_count(void)
{
    f.st0 = st0_base();
    if (f.r != f.eot) {
        f.r++;
    } else if (w.multitrack && f.mt && f.side == 0) {
        f.h ^= 1;
        f.r = 1;
    } else {
        f.c++;
        f.r = 1;
        if (w.multitrack && f.mt) {
            f.h ^= 1;
        }
    }
    rw_result();
}

/* The next sector of a read or write, from f.r */
static void next_sector(void)
{
    bool write = f.op == 0x05 || f.op == 0x09;
    bool deleted_cmd = f.op == 0x0C || f.op == 0x09;
    for (;;) {
        if (f.op == 0x02) {
            if (f.track_index >= f.eot || f.track_index >= f.trk->nsec) {
                end_of_cylinder();
                return;
            }
            f.sec = &f.trk->sec[f.track_index++];
        } else {
            f.sec = find_sector(f.trk);
            if (f.sec == NULL) {
                rw_error(0x40, f.st1 | 0x04, f.st2);    /* no data */
                return;
            }
        }
        if (!write && f.op != 0x02) {
            bool deleted = f.sec->st2 & 0x40;
            if (deleted != deleted_cmd) {
                f.st2 |= 0x40;      /* control mark */
                if (f.sk) {
                    if (f.r == f.eot) {
                        end_of_cylinder();
                        return;
                    }
                    f.r++;
                    continue;
                }
            }
        }
        break;
    }
    light();
    if (disk()->raw) {
        f.xfer = s_sector;
        f.xfer_len = f.sec->len;
        if (write) {
            f.phase = PH_EXEC_WRITE;
            f.msr = 0xB0;
            s_stat.writes[f.drive]++;
        } else {
            if (!disk()->raw->read(f.drive, f.sec->c, f.sec->h, f.sec->r, s_sector)) {
                rw_error(0x40, f.st1 | 0x20, f.st2 | 0x20);     /* data error */
                return;
            }
            f.phase = PH_EXEC_READ;
            f.msr = 0xF0;
            s_stat.reads[f.drive]++;
        }
        f.xfer_pos = 0;
        return;
    }
    if (write) {
        f.xfer = f.trk->data + f.sec->off;
        f.xfer_len = f.sec->len;
        int nominal = f.n ? sector_size(f.n) : f.dtl;
        if (nominal && nominal < f.xfer_len) {
            f.xfer_len = nominal;
        }
        f.sec->st2 = (f.sec->st2 & ~0x40) | (f.op == 0x09 ? 0x40 : 0);
        f.phase = PH_EXEC_WRITE;
        f.msr = 0xB0;
        s_stat.writes[f.drive]++;
    } else {
        sector_window(f.sec, &f.xfer, &f.xfer_len);
        f.phase = PH_EXEC_READ;
        f.msr = 0xF0;
        s_stat.reads[f.drive]++;
    }
    f.xfer_pos = 0;
    if (f.xfer_len == 0) {
        rw_error(0x40, f.st1 | 0x20, f.st2 | 0x20);
    }
}

/* A sector's bytes are through */
static void sector_done(void)
{
    if (f.phase == PH_EXEC_WRITE) {
        if (disk()->raw) {
            if (!disk()->raw->write(f.drive, f.sec->c, f.sec->h, f.sec->r, s_sector)) {
                rw_error(0x40, f.st1 | 0x20, f.st2);
                return;
            }
        } else {
            f.trk->dirty = true;
        }
    }
    uint8_t s1 = f.sec->st1 & 0x25, s2 = f.sec->st2 & 0x61;
    if (f.phase == PH_EXEC_READ && ((s1 & 0x20) || (s2 & 0x20))) {
        rw_error(0x40, f.st1 | s1, f.st2 | s2);     /* data error, as the image says */
        return;
    }
    if ((f.st2 & 0x40) && f.op != 0x02 && !f.sk) {
        rw_error(0x40, f.st1, f.st2);               /* stops after a control mark */
        return;
    }
    if (f.op == 0x02) {
        next_sector();
        return;
    }
    if (f.r == f.eot) {
        if (w.multitrack && f.mt && f.side == 0 && disk()->sides > 1) {
            f.side = 1;             /* MT: on to head 1 */
            f.h ^= 1;
            f.r = 1;
            f.trk = current_track();
            if (f.trk == NULL || f.trk->nsec == 0) {
                rw_error(0x40, 0x01, 0);
                return;
            }
            next_sector();
            return;
        }
        end_of_cylinder();
        return;
    }
    f.r++;
    next_sector();
}

/* DMA: the sectors' bytes through it, until the terminal count, the
 * command's end, or a DMA that doesn't take them (an overrun)
 */
static void dma_sectors(void)
{
    while (f.phase == PH_EXEC_READ || f.phase == PH_EXEC_WRITE) {
        bool tc = false;
        int n = w.dma(f.phase == PH_EXEC_READ, f.xfer + f.xfer_pos, f.xfer_len - f.xfer_pos, &tc);
        f.xfer_pos += n;
        if (f.xfer_pos < f.xfer_len && !tc) {
            rw_error(0x40, f.st1 | 0x10, f.st2);        /* overrun */
            return;
        }
        if (tc) {
            if (f.phase == PH_EXEC_WRITE && disk()->raw &&
                !disk()->raw->write(f.drive, f.sec->c, f.sec->h, f.sec->r, s_sector)) {
                rw_error(0x40, f.st1 | 0x20, f.st2);
                return;
            }
            if (f.phase == PH_EXEC_WRITE && !disk()->raw) {
                f.trk->dirty = true;
            }
            terminal_count();
            return;
        }
        sector_done();
    }
}

static void start_rw(void)
{
    f.mt = f.cmd[0] & 0x80;
    f.sk = f.cmd[0] & 0x20;
    f.c = f.cmd[2];
    f.h = f.cmd[3];
    f.r = f.cmd[4];
    f.n = f.cmd[5];
    f.eot = f.cmd[6];
    f.dtl = f.cmd[8];
    f.st0 = f.st1 = f.st2 = 0;
    f.track_index = 0;
    if (disk() == NULL || !f.motor) {
        rw_error(0x48, 0, 0);           /* not ready */
        return;
    }
    bool write = f.op == 0x05 || f.op == 0x09;
    if (write && disk()->wp) {
        rw_error(0x40, 0x02, 0);        /* not writable */
        return;
    }
    f.trk = current_track();
    if (f.trk == NULL || f.trk->nsec == 0) {
        rw_error(0x40, 0x01, 0);        /* missing address mark */
        return;
    }
    next_sector();
    if (dma_on()) {
        dma_sectors();
    }
}

static void format_done(void)
{
    disk_t *d = disk();
    int cyl = f.pcn[f.drive];
    int nsec = f.fmt_len / 4;
    if (d->raw) {
        /* The layout stays: each 512-byte sector it has is filled */
        memset(s_sector, f.cmd[5], 512);
        for (int i = 0; i < nsec; i++) {
            const uint8_t *id = f.fmt + i * 4;
            if (id[3] == 2 && cyl < d->cyls && f.side < d->sides && id[2] >= 1 &&
                id[2] <= d->trk[cyl][f.side].nsec && !d->raw->write(f.drive, cyl, f.side, id[2], s_sector)) {
                rw_error(0x40, 0x20, 0);
                return;
            }
        }
        s_stat.writes[f.drive] += nsec;
        f.st0 = st0_base();
        f.st1 = f.st2 = 0;
        f.c = f.fmt[(nsec - 1) * 4];
        f.h = f.fmt[(nsec - 1) * 4 + 1];
        f.r = f.fmt[(nsec - 1) * 4 + 2];
        f.n = f.fmt[(nsec - 1) * 4 + 3];
        rw_result();
        return;
    }
    track_t *t = &d->trk[cyl][f.side];
    uint32_t size = 0;
    for (int i = 0; i < nsec; i++) {
        size += sector_size(f.fmt[i * 4 + 3]);
    }
    uint8_t *data = dsk_malloc(size ? size : 1);
    if (data == NULL) {
        rw_error(0x40, 0x10, 0);
        return;
    }
    memset(data, f.cmd[5], size);
    free(t->data);
    uint32_t block = (256 + size + 255) & ~255u;
    if (!t->present || block != t->file_size || cyl >= d->cyls || f.side >= d->sides) {
        d->relayout = true;
    }
    t->present = true;
    t->data = data;
    t->size = size;
    t->n = f.cmd[2];
    t->nsec = nsec;
    t->gap3 = f.cmd[4];
    t->filler = f.cmd[5];
    uint32_t off = 0;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *id = f.fmt + i * 4;
        t->sec[i] = (sector_t){ .c = id[0], .h = id[1], .r = id[2], .n = id[3],
                                .len = sector_size(id[3]), .off = off };
        off += t->sec[i].len;
    }
    t->dirty = true;
    if (!d->relayout) {
        /* same size: the header changes too, so write the whole block */
        d->relayout = true;
    }
    if (cyl >= d->cyls) {
        d->cyls = cyl + 1;
    }
    if (f.side >= d->sides) {
        d->sides = f.side + 1;
    }
    f.st0 = st0_base();
    f.st1 = f.st2 = 0;
    f.c = f.fmt[(nsec - 1) * 4];
    f.h = f.fmt[(nsec - 1) * 4 + 1];
    f.r = f.fmt[(nsec - 1) * 4 + 2];
    f.n = f.fmt[(nsec - 1) * 4 + 3];
    rw_result();
}

static void execute(void)
{
    f.drive = f.cmd[1] & (w.four_units ? 3 : 1);
    f.side = (f.cmd[1] >> 2) & 1;
    switch (f.op) {
    case 0x03:                          /* SPECIFY */
        f.nd = f.cmd[2] & 1;
        idle();
        break;
    case 0x04: {                        /* SENSE DRIVE STATUS */
        disk_t *d = disk();
        uint8_t st3 = st0_base() | 0x20;
        st3 |= f.pcn[f.drive] == 0 ? 0x10 : 0;
        st3 |= (d == NULL || d->wp) ? 0x40 : 0;
        st3 |= w.multitrack && d && d->sides > 1 ? 0x08 : 0;    /* (the PC's drives: two sides) */
        if (d == NULL) {
            st3 &= ~0x20;
        }
        f.res[0] = st3;
        result(1);
        break;
    }
    case 0x07:                          /* RECALIBRATE */
    case 0x0F:                          /* SEEK */
        f.pcn[f.drive] = f.op == 0x07 ? 0 : f.cmd[2];
        if (f.pcn[f.drive] >= MAX_CYL) {
            f.pcn[f.drive] = MAX_CYL - 1;
        }
        /* an empty drive: not ready (the CPC's); the PC's drives are always
         * ready (their line tied high), the units it doesn't have fail
         */
        if (!w.four_units) {
            f.seek_st0[f.drive] = 0x20 | f.drive | (disk() ? 0 : 0x48);
        } else {
            f.seek_st0[f.drive] = 0x20 | f.drive | (f.drive < UPD765_DRIVES ? 0 : 0x50);
        }
        f.seek_pending[f.drive] = true;
        if (w.seek) {
            w.seek(f.drive);
        }
        idle();
        set_int(true);
        break;
    case 0x08:                          /* SENSE INTERRUPT STATUS */
        for (int d = 0; d < UNITS; d++) {
            if (f.seek_pending[d] || f.reset_pending[d]) {
                f.res[0] = f.seek_pending[d] ? f.seek_st0[d] : 0xC0 | d;
                f.seek_pending[d] = f.reset_pending[d] = false;
                f.res[1] = f.pcn[d];
                result(2);
                /* the line falls with each one told (IBM's XT BIOS asks once
                 * after a reset, then waits for the next seek's interrupt)
                 */
                set_int(false);
                return;
            }
        }
        f.res[0] = 0x80;
        result(1);
        break;
    case 0x0A: {                        /* READ ID */
        f.trk = disk() && f.motor ? current_track() : NULL;
        f.c = f.h = f.r = f.n = 0;
        if (disk() == NULL || !f.motor) {
            rw_error(0x48, 0, 0);
        } else if (f.trk == NULL || f.trk->nsec == 0) {
            rw_error(0x40, 0x01, 0);
        } else {
            sector_t *s = &f.trk->sec[disk()->id_index++ % f.trk->nsec];
            f.c = s->c;
            f.h = s->h;
            f.r = s->r;
            f.n = s->n;
            rw_error(0, 0, 0);
        }
        break;
    }
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C:
        start_rw();
        break;
    case 0x0D:                          /* FORMAT TRACK */
        f.c = f.h = f.r = 0;
        f.n = f.cmd[2];
        if (disk() == NULL || !f.motor) {
            rw_error(0x48, 0, 0);
        } else if (disk()->wp) {
            rw_error(0x40, 0x02, 0);
        } else {
            f.fmt_len = (f.cmd[3] > MAX_SECTORS ? MAX_SECTORS : f.cmd[3]) * 4;
            f.xfer_pos = 0;
            if (f.fmt_len == 0) {
                rw_error(0x40, 0, 0);
            } else {
                f.phase = PH_EXEC_WRITE;
                f.msr = 0xB0;
                light();
                if (dma_on()) {
                    bool tc = false;
                    f.xfer_pos = w.dma(false, f.fmt, f.fmt_len, &tc);
                    if (f.xfer_pos >= f.fmt_len) {
                        format_done();
                    } else {
                        rw_error(0x40, 0x10, 0);        /* overrun */
                    }
                }
            }
        }
        break;
    default:                            /* invalid (and SCAN) */
        f.res[0] = 0x80;
        result(1);
        break;
    }
}

uint8_t upd765_status(void)
{
    return f.msr;
}

uint8_t upd765_read(void)
{
    if (f.phase == PH_EXEC_READ) {
        uint8_t v = f.xfer[f.xfer_pos++];
        if (f.xfer_pos >= f.xfer_len) {
            sector_done();
        }
        return v;
    }
    if (f.phase == PH_RESULT) {
        if (f.res_pos == 0 && f.res_len == 7) {
            set_int(false);             /* a data command's: read */
        }
        uint8_t v = f.res[f.res_pos++];
        if (f.res_pos >= f.res_len) {
            idle();
        }
        return v;
    }
    return 0xFF;
}

void upd765_write(uint8_t v)
{
    switch (f.phase) {
    case PH_IDLE:
        f.op = v & 0x1F;
        f.cmd[0] = v;
        f.cmd_pos = 1;
        f.cmd_len = command_length(f.op);
        f.phase = PH_CMD;
        f.msr = 0x90;
        if (f.cmd_len == 1) {
            execute();
        }
        break;
    case PH_CMD:
        f.cmd[f.cmd_pos++] = v;
        if (f.cmd_pos >= f.cmd_len) {
            execute();
        }
        break;
    case PH_EXEC_WRITE:
        if (f.op == 0x0D) {
            f.fmt[f.xfer_pos++] = v;
            if (f.xfer_pos >= f.fmt_len) {
                format_done();
            }
        } else {
            f.xfer[f.xfer_pos++] = v;
            if (f.xfer_pos >= f.xfer_len) {
                sector_done();
            }
        }
        break;
    default:
        break;
    }
}

void upd765_motor(bool on)
{
    f.motor = on;
}

void upd765_frame(void)
{
    if (s_light_frames && --s_light_frames == 0) {
        s_stat.light = -1;
    }
}

void upd765_reset(void)
{
    uint8_t pcn[UNITS];
    memcpy(pcn, f.pcn, sizeof(pcn));
    bool line = f.int_line;
    memset(&f, 0, sizeof(f));
    memcpy(f.pcn, pcn, sizeof(pcn));
    f.int_line = line;
    idle();
    s_stat.light = -1;
    if (w.irq) {
        set_int(false);
        for (int d = 0; d < UNITS; d++) {
            f.reset_pending[d] = true;  /* polling: each unit's ready line "changed" */
        }
        set_int(true);
    }
}

void upd765_wire(const upd765_wiring_t *wiring)
{
    w = *wiring;
}

bool upd765_disk_attach(int drive, const upd765_raw_t *io, int cyls, int heads, int sectors, bool wp,
                        const char *path)
{
    if (drive < 0 || drive >= UPD765_DRIVES || cyls < 1 || heads < 1 || heads > 2 || sectors < 1 ||
        sectors > MAX_SECTORS) {
        return false;
    }
    disk_t *d = dsk_calloc(1, sizeof(disk_t));
    if (d == NULL) {
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->raw = io;
    d->wp = wp;
    d->cyls = cyls > MAX_CYL ? MAX_CYL : cyls;
    d->sides = heads;
    for (int c = 0; c < d->cyls; c++) {
        for (int h = 0; h < heads; h++) {
            track_t *t = &d->trk[c][h];
            t->present = true;
            t->n = 2;
            t->nsec = sectors;
            t->gap3 = 0x54;
            t->filler = 0xF6;
            t->size = sectors * 512;
            for (int i = 0; i < sectors; i++) {
                t->sec[i] = (sector_t){ .c = c, .h = h, .r = i + 1, .n = 2, .len = 512, .off = i * 512 };
            }
        }
    }
    upd765_disk_eject(drive);
    s_disk[drive] = d;
    return true;
}
