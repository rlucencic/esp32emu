/* Commodore D64, D71, D81: blank and partly used disks made here, as the
 * drives format them; build/test_cbm FILE... prints the figures of images
 */
#include "test.h"

#define BLOCK   256

static int sectors(int t)
{
    t = (t - 1) % 35 + 1;
    return t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17;
}

/* A 1541-formatted BAM (and a 1571's side 2), as the drive writes it */
static void format_d64(uint8_t *img, bool d71)
{
    uint8_t *bam = img + 357 * BLOCK;
    bam[0] = 18;
    bam[1] = 1;
    bam[2] = 0x41;
    bam[3] = d71 ? 0x80 : 0;
    for (int t = 1; t <= 35; t++) {
        int n = sectors(t);
        uint32_t bits = (1u << n) - 1;
        if (t == 18) {
            bits &= ~3u;
            n -= 2;
        }
        bam[4 * t] = (uint8_t)n;
        bam[4 * t + 1] = (uint8_t)bits;
        bam[4 * t + 2] = (uint8_t)(bits >> 8);
        bam[4 * t + 3] = (uint8_t)(bits >> 16);
    }
    if (d71) {
        for (int t = 36; t <= 70; t++) {
            bam[0xDD + t - 36] = t == 53 ? 0 : (uint8_t)sectors(t);
        }
    }
}

/* Takes n blocks of track t off the free count */
static void use(uint8_t *img, int t, int n)
{
    uint8_t *bam = img + 357 * BLOCK;
    if (t <= 35) {
        bam[4 * t] -= (uint8_t)n;
    } else {
        bam[0xDD + t - 36] -= (uint8_t)n;
    }
}

static bool space(uint8_t *data, uint64_t size, diskspace_t *s)
{
    test_image_t im = { data, size };
    diskspace_disk_t d = test_disk(&im);
    return diskspace_cbm(&d, s);
}

static void d64_tests(void)
{
    static uint8_t img[197376];
    diskspace_t s;

    memset(img, 0, sizeof(img));
    TEST_CHECK(!space(img, 174848, &s));            /* not formatted */
    format_d64(img, false);
    TEST_CHECK(space(img, 174848, &s));
    TEST_CHECK(s.free == 664 * BLOCK && s.size == 664 * BLOCK);
    TEST_CHECK(space(img, 175531, &s) && s.free == 664 * BLOCK);    /* error bytes */
    TEST_CHECK(!space(img, 174848 + 1, &s));        /* no such size */

    use(img, 1, 21);
    use(img, 17, 5);
    use(img, 18, 3);                                /* the directory's: not counted */
    use(img, 35, 1);
    TEST_CHECK(space(img, 174848, &s) && s.free == (664 - 27) * BLOCK && s.size == 664 * BLOCK);

    /* 40 tracks: plain (the 1541 doesn't see 36-40), SpeedDOS, DolphinDOS */
    TEST_CHECK(space(img, 196608, &s) && s.free == (664 - 27) * BLOCK && s.size == 664 * BLOCK);
    uint8_t *bam = img + 357 * BLOCK;
    for (int t = 0; t < 5; t++) {
        uint8_t *e = bam + 0xC0 + 4 * t;
        e[0] = 17;
        e[1] = 0xFF;
        e[2] = 0xFF;
        e[3] = 0x01;
    }
    bam[0xC0] = 16;                                 /* track 36: one used */
    bam[0xC1] = 0xFE;
    TEST_CHECK(space(img, 196608, &s) && s.free == (664 - 27 + 84) * BLOCK && s.size == (664 + 85) * BLOCK);
    TEST_CHECK(space(img, 197376, &s) && s.free == (664 - 27 + 84) * BLOCK);
    memcpy(bam + 0xAC, bam + 0xC0, 20);
    memset(bam + 0xC0, 0, 20);
    TEST_CHECK(space(img, 196608, &s) && s.free == (664 - 27 + 84) * BLOCK && s.size == (664 + 85) * BLOCK);
    bam[0xAC] = 17;                                 /* count and bitmap disagree: not DolphinDOS */
    TEST_CHECK(space(img, 196608, &s) && s.free == (664 - 27) * BLOCK && s.size == 664 * BLOCK);

    /* nonsense counts */
    bam[4 * 5] = 22;
    TEST_CHECK(!space(img, 174848, &s));
    bam[4 * 5] = 21;
    bam[0] = 0;
    TEST_CHECK(space(img, 174848, &s));             /* (the DOS version still says formatted) */
    bam[2] = 0;
    TEST_CHECK(!space(img, 174848, &s));

    /* C128 CP/M: every block taken, the BAM linked to 1/5 */
    memset(img, 0, sizeof(img));
    format_d64(img, false);
    for (int t = 1; t <= 35; t++) {
        memset(bam + 4 * t, 0, 4);
    }
    bam[0] = 1;
    bam[1] = 5;
    bam[4 * 18] = 17;
    TEST_CHECK(space(img, 174848, &s) && s.free == 0 && s.size == 664 * BLOCK);
}

static void d71_tests(void)
{
    static uint8_t img[351062];
    diskspace_t s;
    memset(img, 0, sizeof(img));
    TEST_CHECK(!space(img, 349696, &s));
    format_d64(img, true);
    TEST_CHECK(space(img, 349696, &s) && s.free == 1328 * BLOCK && s.size == 1328 * BLOCK);
    TEST_CHECK(space(img, 351062, &s) && s.free == 1328 * BLOCK);
    use(img, 36, 21);
    use(img, 70, 2);
    use(img, 1, 1);
    TEST_CHECK(space(img, 349696, &s) && s.free == (1328 - 24) * BLOCK);
    img[357 * BLOCK + 0xDD + 53 - 36] = 19;         /* track 53: not counted */
    TEST_CHECK(space(img, 349696, &s) && s.free == (1328 - 24) * BLOCK);
    img[357 * BLOCK + 3] = 0;                       /* single-sided: side 1 only */
    TEST_CHECK(space(img, 349696, &s) && s.free == (664 - 1) * BLOCK && s.size == 664 * BLOCK);
}

/* A 1581-formatted disk: header 40/0, BAM 40/1 and 40/2 */
static void format_d81(uint8_t *img)
{
    uint8_t *h = img + 39 * 40 * BLOCK;
    h[0] = 40;
    h[1] = 3;
    h[2] = 'D';
    for (int s = 1; s <= 2; s++) {
        uint8_t *b = h + s * BLOCK;
        b[0] = s == 1 ? 40 : 0;
        b[1] = s == 1 ? 2 : 0xFF;
        b[2] = 'D';
        b[3] = 0xBB;
        for (int i = 0; i < 40; i++) {
            uint8_t *e = b + 0x10 + 6 * i;
            int t = (s - 1) * 40 + i + 1;
            e[0] = 40;
            memset(e + 1, 0xFF, 5);
            if (t == 40) {
                e[0] = 36;                          /* header, BAM, first directory sector */
                e[1] = 0xF0;
            }
        }
    }
}

static void d81_tests(void)
{
    static uint8_t img[822400];
    diskspace_t s;
    memset(img, 0, sizeof(img));
    TEST_CHECK(!space(img, 819200, &s));
    format_d81(img);
    TEST_CHECK(space(img, 819200, &s) && s.free == 3160 * BLOCK && s.size == 3160 * BLOCK);
    TEST_CHECK(space(img, 822400, &s) && s.free == 3160 * BLOCK);
    uint8_t *b1 = img + (39 * 40 + 1) * BLOCK, *b2 = b1 + BLOCK;
    b1[0x10] = 0;                                   /* track 1 full */
    b2[0x10 + 6 * 39] = 30;                         /* track 80: 10 used */
    TEST_CHECK(space(img, 819200, &s) && s.free == (3160 - 50) * BLOCK);
    b2[3] = 0;
    TEST_CHECK(!space(img, 819200, &s));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        test_image_t im = test_load(argv[i]);
        diskspace_disk_t d = test_disk(&im);
        diskspace_t s;
        if (diskspace_cbm(&d, &s)) {
            printf("%s: %llu blocks free of %llu\n", argv[i], (unsigned long long)s.free / BLOCK,
                   (unsigned long long)s.size / BLOCK);
        } else {
            printf("%s: not a D64/D71/D81 with a BAM\n", argv[i]);
        }
        free(im.data);
    }
    if (argc > 1) {
        return 0;
    }
    d64_tests();
    d71_tests();
    d81_tests();
    printf("cbm: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
