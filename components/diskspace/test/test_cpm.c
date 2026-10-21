/* CP/M: AMSDOS's data, system and IBM formats, +3DOS, a disk of 16-bit
 * block numbers; blank, then with files
 */
#include "test.h"

static uint8_t s_img[160 * 9 * 512];

static const diskspace_cpm_geometry_t data = { 512, 9, 40, 0, 1024, 64 };
static const diskspace_cpm_geometry_t sys = { 512, 9, 40, 2, 1024, 64 };
static const diskspace_cpm_geometry_t ibm = { 512, 8, 40, 1, 1024, 64 };
static const diskspace_cpm_geometry_t plus3 = { 512, 9, 40, 1, 1024, 64 };
static const diskspace_cpm_geometry_t pcw720 = { 512, 9, 160, 1, 2048, 256 };

/* Formatted: track t's sector s at (t * sectors + s) * sector_size */
static test_image_t blank(const diskspace_cpm_geometry_t *g)
{
    test_image_t im = { s_img, (uint64_t)g->tracks * g->sectors * g->sector_size };
    memset(s_img, 0xE5, im.size);
    return im;
}

/* Directory entry i: user (0xE5 deleted), name, its blocks */
static uint8_t *entry(const diskspace_cpm_geometry_t *g, int i, uint8_t user, const char *name,
                      const uint16_t *blocks, int n)
{
    uint64_t total = (uint64_t)(g->tracks - g->reserved_tracks) * g->sectors * g->sector_size / g->block_size;
    uint8_t *e = s_img + (uint64_t)g->reserved_tracks * g->sectors * g->sector_size + i * 32;
    memset(e, 0, 32);
    e[0] = user;
    memset(e + 1, ' ', 11);
    memcpy(e + 1, name, strlen(name));
    e[15] = n * g->block_size / 128 > 128 ? 128 : n * g->block_size / 128;
    for (int k = 0; k < n; k++) {
        if (total > 256) {
            e[16 + 2 * k] = blocks[k] & 0xFF;
            e[17 + 2 * k] = blocks[k] >> 8;
        } else {
            e[16 + k] = blocks[k];
        }
    }
    return e;
}

static bool count(const diskspace_cpm_geometry_t *g, test_image_t *im, diskspace_t *s)
{
    diskspace_disk_t d = test_disk(im);
    return diskspace_cpm(&d, g, s);
}

static void blanks(void)
{
    diskspace_t s;
    test_image_t im = blank(&data);
    TEST_CHECK(count(&data, &im, &s) && s.free == 178 * 1024 && s.size == 180 * 1024);
    im = blank(&sys);
    TEST_CHECK(count(&sys, &im, &s) && s.free == 169 * 1024 && s.size == 171 * 1024);
    im = blank(&ibm);
    TEST_CHECK(count(&ibm, &im, &s) && s.free == 154 * 1024 && s.size == 156 * 1024);
    im = blank(&plus3);
    TEST_CHECK(count(&plus3, &im, &s) && s.free == 173 * 1024 && s.size == 175 * 1024);
    /* 159 tracks of 4.5 KB: 357 blocks of 2 KB, 4 for 256 entries */
    im = blank(&pcw720);
    TEST_CHECK(count(&pcw720, &im, &s) && s.free == 353 * 2048 && s.size == 357 * 2048);
}

static void files(void)
{
    diskspace_t s;
    test_image_t im = blank(&data);
    static const uint16_t hello[] = { 2, 3, 4 };
    static const uint16_t big0[] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
    static const uint16_t big1[] = { 21, 22, 23, 24 };
    static const uint16_t gone[] = { 30, 31, 32, 33, 34 };
    static const uint16_t other[] = { 4, 25, 1 };      /* one shared, one a directory block */
    entry(&data, 0, 0, "HELLO   BAS", hello, 3);
    entry(&data, 1, 0, "BIG     BIN", big0, 16);
    entry(&data, 2, 0, "BIG     BIN", big1, 4)[12] = 1;
    entry(&data, 3, 0xE5, "OLD     BIN", gone, 5);
    uint8_t *e = entry(&data, 4, 15, "OTHER   ", other, 3);
    e[9] |= 0x80;                                       /* read-only */
    TEST_CHECK(count(&data, &im, &s) && s.free == (178 - 24) * 1024 && s.size == 180 * 1024);

    /* CP/M 3's label, timestamps and passwords take no blocks */
    im = blank(&sys);
    static const uint16_t junk[] = { 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                                     0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0 };
    static const uint16_t prog[] = { 2, 3, 4, 5, 6, 7, 8, 9 };
    entry(&sys, 0, 0x20, "DISKLABEL  ", junk, 16);
    entry(&sys, 1, 0, "PROG    COM", prog, 8);
    entry(&sys, 2, 0x10, "PROG    COM", junk, 16);
    entry(&sys, 3, 0x21, "", junk, 16);
    TEST_CHECK(count(&sys, &im, &s) && s.free == 161 * 1024 && s.size == 171 * 1024);

    /* +3DOS: a file in the last block */
    im = blank(&plus3);
    static const uint16_t last[] = { 174 };
    entry(&plus3, 63, 3, "LAST", last, 1);
    TEST_CHECK(count(&plus3, &im, &s) && s.free == 172 * 1024);

    /* 16-bit numbers */
    im = blank(&pcw720);
    static const uint16_t wide[] = { 4, 300, 356 };
    entry(&pcw720, 0, 0, "WIDE    DAT", wide, 3);
    TEST_CHECK(count(&pcw720, &im, &s) && s.free == 350 * 2048 && s.size == 357 * 2048);
}

static void refused(void)
{
    diskspace_t s;
    /* a block past the end: not a CP/M directory */
    test_image_t im = blank(&data);
    static const uint16_t past[] = { 2, 180 };
    entry(&data, 0, 0, "PAST", past, 2);
    TEST_CHECK(!count(&data, &im, &s));
    im = blank(&pcw720);
    static const uint16_t past16[] = { 357 };
    entry(&pcw720, 5, 0, "PAST", past16, 1);
    TEST_CHECK(!count(&pcw720, &im, &s));
    /* zeros: names that aren't names */
    im = blank(&data);
    memset(s_img, 0, im.size);
    TEST_CHECK(!count(&data, &im, &s));
    /* a record count over 128 */
    im = blank(&data);
    entry(&data, 0, 0, "BIG", NULL, 0)[15] = 0x81;
    TEST_CHECK(!count(&data, &im, &s));
    /* an image shorter than its directory */
    im = blank(&sys);
    im.size = 2 * 9 * 512 + 1000;
    TEST_CHECK(!count(&sys, &im, &s));
    /* geometries that aren't */
    im = blank(&data);
    diskspace_cpm_geometry_t g = data;
    g.block_size = 1000;
    TEST_CHECK(!count(&g, &im, &s));
    g = data;
    g.reserved_tracks = 40;
    TEST_CHECK(!count(&g, &im, &s));
    g = data;
    g.dir_entries = 180 * 1024 / 32;
    TEST_CHECK(!count(&g, &im, &s));
    g = data;
    g.sector_size = 0;
    TEST_CHECK(!count(&g, &im, &s));
}

int main(void)
{
    blanks();
    files();
    refused();
    printf("cpm: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
