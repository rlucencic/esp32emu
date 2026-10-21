/* FAT12/16/32: volumes made here (PC and Atari ST floppies, a FAT16 and a
 * FAT32 volume, a partitioned hard disk), counted exactly; then any image
 * files given (build/test_fat FILE...: their figures printed)
 */
#include "test.h"

typedef struct {
    uint32_t bps, spc, reserved, fats, root, total, fat_sectors;
    int bits;                       /* how its entries are written */
    bool st;                        /* an Atari ST's: no jump, no 0x55AA */
} vol_t;

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* Cluster n's entry in every FAT */
static void set(uint8_t *v, const vol_t *g, uint32_t n, uint32_t value)
{
    for (uint32_t f = 0; f < g->fats; f++) {
        uint8_t *fat = v + (g->reserved + f * g->fat_sectors) * g->bps;
        if (g->bits == 12) {
            uint8_t *p = fat + n * 3 / 2;
            if (n & 1) {
                p[0] = (p[0] & 0x0F) | (value << 4 & 0xF0);
                p[1] = value >> 4;
            } else {
                p[0] = value;
                p[1] = (p[1] & 0xF0) | (value >> 8 & 0x0F);
            }
        } else if (g->bits == 16) {
            put16(fat + 2 * n, value);
        } else {
            put32(fat + 4 * n, value);
        }
    }
}

/* A blank volume at v: boot sector, FATs with their first two entries */
static void format(uint8_t *v, const vol_t *g)
{
    memset(v, 0, (size_t)g->total * g->bps);
    if (g->st) {
        v[0] = 0x60;                /* BRA.S, or nothing at all */
        v[1] = 0x38;
    } else {
        v[0] = 0xEB;
        v[1] = 0x3C;
        v[2] = 0x90;
        v[510] = 0x55;
        v[511] = 0xAA;
    }
    put16(v + 11, g->bps);
    v[13] = g->spc;
    put16(v + 14, g->reserved);
    v[16] = g->fats;
    put16(v + 17, g->root);
    if (g->total < 65536 && g->bits != 32) {
        put16(v + 19, g->total);
    } else {
        put32(v + 32, g->total);
    }
    v[21] = 0xF8;
    if (g->bits == 32) {
        put32(v + 36, g->fat_sectors);
        put32(v + 44, 2);           /* root directory's cluster */
        put16(v + 48, 1);           /* FSInfo */
        put16(v + 50, 6);           /* backup boot sector */
    } else {
        put16(v + 22, g->fat_sectors);
    }
    set(v, g, 0, 0x0FFFFFF8);
    set(v, g, 1, 0x0FFFFFFF);
}

/* FAT32's FSInfo with a free count */
static void fsinfo(uint8_t *v, const vol_t *g, uint32_t free_count)
{
    uint8_t *s = v + g->bps;
    put32(s, 0x41615252);
    put32(s + 484, 0x61417272);
    put32(s + 488, free_count);
    put32(s + 492, 3);
    put32(s + 508, 0xAA550000);
}

static bool count(uint8_t *data, uint64_t size, bool disk, diskspace_t *s)
{
    test_image_t im = { data, size };
    diskspace_disk_t d = test_disk(&im);
    return disk ? diskspace_fat_disk(&d, s) : diskspace_fat(&d, 0, s);
}

/* The volume counted alone and as a disk, against clusters and used */
static void check(uint8_t *v, const vol_t *g, uint32_t clusters, uint32_t used)
{
    uint64_t cluster = (uint64_t)g->bps * g->spc, size = (uint64_t)g->total * g->bps;
    for (int disk = 0; disk < 2; disk++) {
        diskspace_t s = { 0, 0 };
        bool ok = count(v, size, disk, &s);
        TEST_CHECK(ok);
        TEST_CHECK(s.size == clusters * cluster);
        TEST_CHECK(s.free == (clusters - used) * cluster);
        if (!ok || s.free != (clusters - used) * cluster || s.size != clusters * cluster) {
            fprintf(stderr, "  %u-bit, %u sectors%s: %llu free of %llu, expected %llu of %llu\n", g->bits, g->total,
                    disk ? " (disk)" : "", (unsigned long long)s.free, (unsigned long long)s.size,
                    (unsigned long long)((clusters - used) * cluster), (unsigned long long)(clusters * cluster));
        }
    }
}

static uint8_t *image(size_t bytes)
{
    uint8_t *p = calloc(1, bytes);
    if (p == NULL) {
        perror("calloc");
        exit(1);
    }
    return p;
}

static void floppies(void)
{
    uint8_t *v = image(1474560);

    /* PC 720 KB: (1440 - 1 - 2 x 3 - 7) / 2 clusters */
    vol_t pc720 = { 512, 2, 1, 2, 112, 1440, 3, 12, false };
    format(v, &pc720);
    check(v, &pc720, 713, 0);
    uint32_t used = 0;
    for (uint32_t n = 2; n < 12; n++, used++) {
        set(v, &pc720, n, n + 1);
    }
    set(v, &pc720, 340, 0xFFF);     /* entries across the first sector's end */
    set(v, &pc720, 341, 0xFFF);
    set(v, &pc720, 342, 0xFFF);
    set(v, &pc720, 714, 0xFFF);     /* the last cluster */
    used += 4;
    check(v, &pc720, 713, used);

    /* PC 1.44 MB: (2880 - 1 - 2 x 9 - 14) clusters of one sector */
    vol_t pc144 = { 512, 1, 1, 2, 224, 2880, 9, 12, false };
    format(v, &pc144);
    check(v, &pc144, 2847, 0);
    used = 0;
    static const uint32_t marks[] = { 2, 3, 340, 341, 342, 682, 683, 1023, 1024, 2000, 2847, 2848 };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++, used++) {
        set(v, &pc144, marks[i], i & 1 ? 0xFF7 : 0xFFF);    /* bad clusters aren't free either */
    }
    memset(v + (1 + 9) * 512, 0xFF, 9 * 512);              /* the second FAT isn't read */
    check(v, &pc144, 2847, used);

    /* Atari ST, as TOS formats them: no jump, no 0x55AA; double sided:
     * (1440 - 1 - 2 x 5 - 7) / 2, single sided (720 - 18) / 2 clusters
     */
    vol_t st_ds = { 512, 2, 1, 2, 112, 1440, 5, 12, true };
    format(v, &st_ds);
    v[0] = v[1] = 0;
    check(v, &st_ds, 711, 0);
    for (uint32_t n = 2; n < 302; n++) {
        set(v, &st_ds, n, n == 301 ? 0xFFF : n + 1);
    }
    check(v, &st_ds, 711, 300);
    diskspace_t s;
    TEST_CHECK(!count(v, 1440 * 512 - 512, false, &s));    /* the BPB says more than there is */

    vol_t st_ss = { 512, 2, 1, 2, 112, 720, 5, 12, true };
    format(v, &st_ss);
    check(v, &st_ss, 351, 0);
    set(v, &st_ss, 352, 0xFFF);
    set(v, &st_ss, 100, 0xFFF);
    check(v, &st_ss, 351, 2);

    /* Not FAT: nonsense in the BPB */
    format(v, &pc144);
    v[11] = 0xE8;                   /* 1000 bytes a sector */
    v[12] = 0x03;
    TEST_CHECK(!count(v, 1474560, false, &s));
    format(v, &pc144);
    v[13] = 3;                      /* sectors per cluster */
    TEST_CHECK(!count(v, 1474560, false, &s));
    format(v, &pc144);
    v[16] = 3;                      /* FATs */
    TEST_CHECK(!count(v, 1474560, false, &s));
    format(v, &pc144);
    v[14] = v[15] = 0;              /* reserved sectors */
    TEST_CHECK(!count(v, 1474560, false, &s));
    format(v, &pc144);
    put16(v + 22, 1);               /* a FAT too small for its clusters */
    TEST_CHECK(!count(v, 1474560, false, &s));
    format(v, &pc144);
    put16(v + 17, 0);               /* FAT12 without a root directory */
    TEST_CHECK(!count(v, 1474560, false, &s));
    free(v);
}

/* The FAT kind by the cluster count only: 4084 clusters FAT12, 4085 FAT16 */
static void kinds(void)
{
    uint8_t *v = image(4200 * 512);
    vol_t f12 = { 512, 1, 1, 1, 16, 1 + 12 + 1 + 4084, 12, 12, false };
    format(v, &f12);
    set(v, &f12, 4085, 0xFFF);
    set(v, &f12, 3, 0xFFF);
    check(v, &f12, 4084, 2);
    vol_t f16 = { 512, 1, 1, 1, 16, 1 + 16 + 1 + 4085, 16, 16, false };
    format(v, &f16);
    set(v, &f16, 4086, 0xFFFF);
    set(v, &f16, 3, 0xFFFF);
    check(v, &f16, 4085, 2);
    free(v);
}

static void fat16(void)
{
    /* 8 MB: 16384 - 1 - 2 x 64 - 32 clusters of one sector */
    vol_t g = { 512, 1, 1, 2, 512, 16384, 64, 16, false };
    uint8_t *v = image(16384 * 512);
    format(v, &g);
    check(v, &g, 16223, 0);
    uint32_t used = 0;
    for (uint32_t n = 2; n < 102; n++, used++) {
        set(v, &g, n, n == 101 ? 0xFFFF : n + 1);
    }
    set(v, &g, 255, 0xFFFF);        /* a sector's last entry, the next one's first */
    set(v, &g, 256, 0xFFFF);
    set(v, &g, 16224, 0xFFFF);      /* the last cluster */
    used += 3;
    check(v, &g, 16223, used);

    /* 2 KB clusters, more than 65535 sectors (the 32-bit count) */
    vol_t big = { 512, 4, 4, 2, 512, 100000, 98, 16, false };
    uint8_t *b = image(100000 * 512);
    format(b, &big);
    set(b, &big, 2, 0xFFFF);
    check(b, &big, (100000 - 4 - 196 - 32) / 4, 1);
    free(b);
    free(v);
}

static void fat32(void)
{
    /* 40 MB: 81920 - 32 - 2 x 640 clusters of one sector */
    vol_t g = { 512, 1, 32, 2, 0, 81920, 640, 32, false };
    uint8_t *v = image(81920 * 512);
    format(v, &g);
    set(v, &g, 2, 0x0FFFFFFF);      /* the root directory */
    for (uint32_t n = 100; n < 200; n++) {
        set(v, &g, n, n == 199 ? 0x0FFFFFFF : n + 1);
    }
    set(v, &g, 80609, 0x0FFFFFFF);  /* the last cluster */
    set(v, &g, 500, 0xF0000000);    /* the top four bits aren't the entry's */
    check(v, &g, 80608, 102);       /* no FSInfo: counted */
    fsinfo(v, &g, 12345);
    check(v, &g, 80608, 80608 - 12345);     /* FSInfo's count */
    fsinfo(v, &g, 0xFFFFFFFF);
    check(v, &g, 80608, 102);       /* not known */
    fsinfo(v, &g, 80609);
    check(v, &g, 80608, 102);       /* more than there are */
    fsinfo(v, &g, 777);
    put32(v + 512 + 484, 0);
    check(v, &g, 80608, 102);       /* a signature missing */

    put16(v + 17, 512);             /* root entries on FAT32: not FAT */
    diskspace_t s;
    TEST_CHECK(!count(v, 81920 * 512, false, &s));
    free(v);
}

static void put_part(uint8_t *table, int i, uint8_t type, uint32_t start, uint32_t sectors)
{
    uint8_t *p = table + 446 + 16 * i;
    p[0] = i == 0 ? 0x80 : 0;
    p[4] = type;
    put32(p + 8, start);
    put32(p + 12, sectors);
    table[510] = 0x55;
    table[511] = 0xAA;
}

/* An MBR: a FAT16, a Linux partition, an extended one with a FAT12 and a
 * FAT16 inside (Nextor's FDISK makes its later partitions logical ones)
 */
static void hard_disk(void)
{
    vol_t a = { 512, 1, 1, 2, 512, 16384, 64, 16, false };
    vol_t b = { 512, 4, 1, 2, 512, 8000, 6, 12, false };
    vol_t c = { 512, 2, 1, 2, 512, 20000, 40, 16, false };
    uint32_t a_at = 63, linux_at = a_at + a.total, ext = linux_at + 1000;
    uint32_t b_at = ext + 63, ebr2 = b_at + b.total, c_at = ebr2 + 63, end = c_at + c.total;
    uint8_t *disk = image((size_t)end * 512);
    format(disk + (size_t)a_at * 512, &a);
    format(disk + (size_t)b_at * 512, &b);
    format(disk + (size_t)c_at * 512, &c);
    set(disk + (size_t)a_at * 512, &a, 2, 0xFFFF);
    set(disk + (size_t)b_at * 512, &b, 2, 0xFFF);
    set(disk + (size_t)b_at * 512, &b, 3, 0xFFF);
    set(disk + (size_t)c_at * 512, &c, 9, 0xFFFF);
    put_part(disk, 0, 0x06, a_at, a.total);
    put_part(disk, 1, 0x83, linux_at, 1000);
    put_part(disk, 2, 0x05, ext, end - ext);
    put_part(disk + (size_t)ext * 512, 0, 0x01, b_at - ext, b.total);
    put_part(disk + (size_t)ext * 512, 1, 0x05, ebr2 - ext, end - ebr2);
    disk[(size_t)ext * 512 + 446] = 0;
    put_part(disk + (size_t)ebr2 * 512, 0, 0x0E, c_at - ebr2, c.total);
    disk[(size_t)ebr2 * 512 + 446] = 0;

    uint32_t ca = 16223, cb = (8000 - 1 - 12 - 32) / 4, cc = (20000 - 1 - 80 - 32) / 2;
    diskspace_t s = { 0, 0 };
    TEST_CHECK(cb < 4085 && cc >= 4085);
    TEST_CHECK(count(disk, (uint64_t)end * 512, true, &s));
    TEST_CHECK(s.size == (uint64_t)ca * 512 + (uint64_t)cb * 2048 + (uint64_t)cc * 1024);
    TEST_CHECK(s.free == (uint64_t)(ca - 1) * 512 + (uint64_t)(cb - 2) * 2048 + (uint64_t)(cc - 1) * 1024);

    /* The primary partition alone */
    disk[446 + 32 + 4] = 0;
    TEST_CHECK(count(disk, (uint64_t)end * 512, true, &s));
    TEST_CHECK(s.size == (uint64_t)ca * 512 && s.free == (uint64_t)(ca - 1) * 512);

    /* A partition past the image's end: not counted, nothing left */
    put32(disk + 446 + 8, end);
    TEST_CHECK(!count(disk, (uint64_t)end * 512, true, &s));
    free(disk);
}

int main(int argc, char **argv)
{
    floppies();
    kinds();
    fat16();
    fat32();
    hard_disk();
    for (int i = 1; i < argc; i++) {
        test_image_t im = test_load(argv[i]);
        diskspace_disk_t d = test_disk(&im);
        diskspace_t s;
        if (diskspace_fat_disk(&d, &s)) {
            printf("%s: %llu bytes free of %llu\n", argv[i], (unsigned long long)s.free, (unsigned long long)s.size);
        } else {
            printf("%s: not FAT\n", argv[i]);
        }
        free(im.data);
    }
    printf("fat: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
