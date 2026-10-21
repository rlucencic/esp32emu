/* Tandy Color Computer: RS-DOS (Disk BASIC) and OS-9 disks made here as
 * DSKINI and OS-9's format leave them; build/test_coco FILE... prints the
 * figures of images (256-byte sectors from 0: a JVC header is skipped)
 */
#include "test.h"

#define SECTOR  256
#define TRACK   (18 * SECTOR)
#define GRANULE (9 * SECTOR)

static uint8_t img[0x10000 * SECTOR];

/* DSKINI: the granule table's first n bytes 0xFF, the rest 0 */
static void dskini(int n)
{
    memset(img, 0xFF, 80 * TRACK);
    uint8_t *fat = img + 17 * TRACK + SECTOR;
    memset(fat, 0, SECTOR);
    memset(fat, 0xFF, (size_t)n);
}

static bool rsdos(uint64_t size, diskspace_t *s)
{
    test_image_t im = { img, size };
    diskspace_disk_t d = test_disk(&im);
    return diskspace_rsdos(&d, s);
}

static bool os9(uint64_t size, diskspace_t *s)
{
    test_image_t im = { img, size };
    diskspace_disk_t d = test_disk(&im);
    return diskspace_os9(&d, s);
}

/* OS-9's format: LSN 0, the bitmap from LSN 1 (bits past the end set),
 * the root directory's descriptor and its sectors in use
 */
static void format_os9(uint32_t tot, uint32_t bit, uint32_t used_sectors)
{
    memset(img, 0, (size_t)tot * SECTOR);
    uint32_t clusters = tot / bit, map = (clusters + 7) / 8;
    uint32_t map_sectors = (map + SECTOR - 1) / SECTOR;
    img[0] = (uint8_t)(tot >> 16);
    img[1] = (uint8_t)(tot >> 8);
    img[2] = (uint8_t)tot;
    img[3] = 18;
    img[4] = (uint8_t)(map >> 8);
    img[5] = (uint8_t)map;
    img[6] = (uint8_t)(bit >> 8);
    img[7] = (uint8_t)bit;
    uint32_t dir = 1 + map_sectors;
    img[8] = (uint8_t)(dir >> 16);
    img[9] = (uint8_t)(dir >> 8);
    img[10] = (uint8_t)dir;
    memcpy(img + 0x1F, "BLANK", 5);
    img[0x1F + 4] |= 0x80;
    uint8_t *bm = img + SECTOR;
    for (uint32_t c = 0; c < map_sectors * SECTOR * 8; c++) {
        if (c < (used_sectors + bit - 1) / bit || c >= clusters) {
            bm[c / 8] |= (uint8_t)(0x80 >> (c % 8));
        }
    }
}

static void rsdos_tests(void)
{
    diskspace_t s;
    uint64_t t35 = 35 * TRACK, t40 = 40 * TRACK, t80 = 80 * TRACK;

    memset(img, 0, 80 * TRACK);
    TEST_CHECK(!rsdos(t35, &s));                    /* zeros: granule 0 points to itself */

    /* DSKINI on 35 tracks: 68 granules free; on a 40-track image too */
    dskini(68);
    TEST_CHECK(rsdos(t35, &s) && s.free == 68 * GRANULE && s.size == 68 * GRANULE);
    TEST_CHECK(rsdos(t40, &s) && s.free == 68 * GRANULE && s.size == 68 * GRANULE);
    TEST_CHECK(!rsdos(t35 - TRACK, &s));            /* too short */

    /* a file of granules 32 -> 33 -> 34 (last, 5 sectors), one of 67 */
    uint8_t *fat = img + 17 * TRACK + SECTOR;
    fat[32] = 33;
    fat[33] = 34;
    fat[34] = 0xC5;
    fat[67] = 0xC9;
    TEST_CHECK(rsdos(t35, &s) && s.free == 64 * GRANULE && s.size == 68 * GRANULE);
    fat[10] = 33;                                   /* two granules pointing to 33 */
    TEST_CHECK(!rsdos(t35, &s));
    fat[10] = 10;                                   /* one to itself */
    TEST_CHECK(!rsdos(t35, &s));
    fat[10] = 0x80;
    TEST_CHECK(!rsdos(t35, &s));

    /* 40 and 80 track DOSes: 78 and 158 granules */
    dskini(78);
    TEST_CHECK(rsdos(t40, &s) && s.free == 78 * GRANULE && s.size == 78 * GRANULE);
    TEST_CHECK(rsdos(t35, &s) && s.free == 68 * GRANULE);  /* a 35-track image: only 68 */
    fat[77] = 0xC1;
    TEST_CHECK(rsdos(t80, &s) && s.free == 77 * GRANULE && s.size == 78 * GRANULE);
    dskini(158);
    TEST_CHECK(rsdos(t80, &s) && s.free == 158 * GRANULE && s.size == 158 * GRANULE);

    /* an unformatted image of 0xFF bytes reads as an empty RS-DOS disk */
    memset(img, 0xFF, 80 * TRACK);
    TEST_CHECK(rsdos(t35, &s) && s.free == 68 * GRANULE);
    TEST_CHECK(!os9(t35, &s));
}

static void os9_tests(void)
{
    diskspace_t s;

    /* 35 tracks single sided: 630 sectors, 11 in use (LSN 0, bitmap, root) */
    format_os9(630, 1, 11);
    TEST_CHECK(os9(630 * SECTOR, &s) && s.free == 619 * SECTOR && s.size == 630 * SECTOR);
    TEST_CHECK(!os9(629 * SECTOR, &s));             /* shorter than DD.TOT */
    img[SECTOR + 20] = 0x0F;                        /* 4 more in use */
    TEST_CHECK(os9(630 * SECTOR, &s) && s.free == 615 * SECTOR);
    TEST_CHECK(!rsdos(630 * SECTOR, &s));           /* its track 17 isn't a granule table */

    /* 40 tracks double sided: 1440 sectors */
    format_os9(1440, 1, 11);
    TEST_CHECK(os9(1440 * SECTOR, &s) && s.free == 1429 * SECTOR && s.size == 1440 * SECTOR);

    /* a hard disk: 4 sectors a cluster, a bitmap of several sectors, an
     * image longer than the volume
     */
    format_os9(0x10000, 4, 100);
    TEST_CHECK(os9(0x10000 * SECTOR + 12345, &s) && s.free == (0x4000ull - 25) * 4 * SECTOR &&
               s.size == 0x10000ull * SECTOR);
    img[SECTOR + 2047] = 0xFF;                      /* the last 8 clusters, in the bitmap's last byte */
    TEST_CHECK(os9(0x10000 * SECTOR, &s) && s.free == (0x4000ull - 33) * 4 * SECTOR);

    /* DD.TOT not a multiple of DD.BIT: the clusters it fills, bits past them ignored */
    format_os9(1001, 2, 12);
    img[SECTOR + 62] &= 0xF0;                       /* (clusters 500-503 past the end: cleared) */
    TEST_CHECK(os9(1001 * SECTOR, &s) && s.free == (500 - 6) * 2 * SECTOR && s.size == 1001 * SECTOR);

    /* nonsense figures */
    format_os9(630, 1, 11);
    img[7] = 3;                                     /* DD.BIT not a power of 2 */
    TEST_CHECK(!os9(630 * SECTOR, &s));
    img[7] = 1;
    img[5] = 50;                                    /* bitmap too short for DD.TOT */
    TEST_CHECK(!os9(630 * SECTOR, &s));
    img[5] = 79;
    img[10] = 1;                                    /* root directory on the bitmap */
    TEST_CHECK(!os9(630 * SECTOR, &s));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        test_image_t im = test_load(argv[i]);
        test_image_t body = { im.data + im.size % SECTOR, im.size - im.size % SECTOR };
        diskspace_disk_t d = test_disk(&body);
        diskspace_t s;
        if (diskspace_os9(&d, &s)) {
            printf("%s: OS-9, %llu bytes free of %llu\n", argv[i], (unsigned long long)s.free,
                   (unsigned long long)s.size);
        } else if (diskspace_rsdos(&d, &s)) {
            printf("%s: RS-DOS, %llu granules free of %llu\n", argv[i], (unsigned long long)s.free / GRANULE,
                   (unsigned long long)s.size / GRANULE);
        } else {
            printf("%s: neither OS-9 nor RS-DOS\n", argv[i]);
        }
        free(im.data);
    }
    if (argc > 1) {
        return 0;
    }
    rsdos_tests();
    os9_tests();
    printf("coco: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
