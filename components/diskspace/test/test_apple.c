/* MFS, HFS, HFS+ (raw and wrapped), Apple partition maps, ProDOS and DOS
 * 3.3 images made here; files given as arguments are tried with each
 * counter and the answers printed
 */
#include "test.h"

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static test_image_t image(uint64_t size)
{
    test_image_t im = { calloc(size, 1), size };
    return im;
}

/* An MFS or HFS master directory block for a volume at base */
static void mdb(uint8_t *base, uint16_t sig, uint32_t blocks, uint32_t block_size, uint32_t first, uint32_t free_blocks)
{
    uint8_t *m = base + 1024;
    put16(m, sig);
    put16(m + 18, blocks);
    put32(m + 20, block_size);
    put16(m + 28, first);
    put16(m + 34, free_blocks);
}

static void hfs_plus(uint8_t *base, uint16_t sig, uint32_t block_size, uint32_t total, uint32_t free_blocks)
{
    uint8_t *h = base + 1024;
    put16(h, sig);
    put16(h + 2, sig == 0x482B ? 4 : 5);
    put32(h + 40, block_size);
    put32(h + 44, total);
    put32(h + 48, free_blocks);
}

static void test_mfs_hfs(void)
{
    diskspace_t s;
    /* MFS, 400K: 391 blocks of 1 KB after 12 sectors */
    test_image_t im = image(400 * 1024);
    diskspace_disk_t d = test_disk(&im);
    mdb(im.data, 0xD2D7, 391, 1024, 12, 100);
    TEST_CHECK(diskspace_hfs(&d, 0, &s) && s.free == 100 * 1024 && s.size == 391 * 1024);
    TEST_CHECK(diskspace_mac_disk(&d, &s) && s.free == 100 * 1024);
    TEST_CHECK(!diskspace_prodos(&d, 0, &s) && !diskspace_dos33(&d, &s));
    free(im.data);

    /* HFS, 800K: 1594 blocks of 512 bytes after 6 sectors */
    im = image(800 * 1024);
    d = test_disk(&im);
    mdb(im.data, 0x4244, 1594, 512, 6, 1203);
    TEST_CHECK(diskspace_hfs(&d, 0, &s) && s.free == 1203 * 512 && s.size == 1594 * 512);
    TEST_CHECK(diskspace_mac_disk(&d, &s) && s.free == 1203 * 512 && s.size == 1594 * 512);
    d.size = 0;                                 /* size not known: still read */
    TEST_CHECK(diskspace_hfs(&d, 0, &s) && s.free == 1203 * 512);
    d.size = im.size;
    /* insane figures */
    mdb(im.data, 0x4244, 1594, 512, 6, 1595);  /* more free than there is */
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    mdb(im.data, 0x4244, 1594, 1000, 6, 10);   /* not a multiple of 512 */
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    mdb(im.data, 0x4244, 1600, 512, 6, 10);    /* past the image's end */
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    mdb(im.data, 0x4244, 0, 512, 6, 0);
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    mdb(im.data, 0x4243, 1594, 512, 6, 10);    /* no signature */
    TEST_CHECK(!diskspace_hfs(&d, 0, &s) && !diskspace_mac_disk(&d, &s));
    /* at an offset (a volume after a header) */
    memset(im.data, 0, im.size);
    mdb(im.data + 84, 0x4244, 1000, 512, 6, 7);
    TEST_CHECK(diskspace_hfs(&d, 84, &s) && s.free == 7 * 512 && s.size == 1000 * 512);
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    free(im.data);
}

static void test_hfs_plus(void)
{
    diskspace_t s;
    /* HFS+, 2 MB: 512 blocks of 4 KB */
    test_image_t im = image(2 * 1024 * 1024);
    diskspace_disk_t d = test_disk(&im);
    hfs_plus(im.data, 0x482B, 4096, 512, 301);
    TEST_CHECK(diskspace_hfs(&d, 0, &s) && s.free == 301 * 4096 && s.size == 2 * 1024 * 1024);
    TEST_CHECK(diskspace_mac_disk(&d, &s) && s.free == 301 * 4096);
    hfs_plus(im.data, 0x4858, 4096, 512, 0);   /* HFSX, full */
    TEST_CHECK(diskspace_hfs(&d, 0, &s) && s.free == 0 && s.size == 2 * 1024 * 1024);
    hfs_plus(im.data, 0x482B, 4096, 513, 1);   /* past the end */
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    hfs_plus(im.data, 0x482B, 4096, 512, 513);
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    hfs_plus(im.data, 0x482B, 0, 512, 1);
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    free(im.data);

    /* An HFS wrapper (4 KB allocation blocks after 4 sectors) with the HFS+
     * volume from its block 3: 3000 blocks of 4 KB, 1234 free
     */
    uint32_t first = 4, start = 3, count = 3000;
    uint64_t embedded = first * 512 + start * 4096;
    im = image(embedded + count * 4096ull + 4096);
    d = test_disk(&im);
    mdb(im.data, 0x4244, count + 4, 4096, first, 0);
    uint8_t *m = im.data + 1024;
    put16(m + 124, 0x482B);
    put16(m + 126, start);
    put16(m + 128, count);
    hfs_plus(im.data + embedded, 0x482B, 4096, count, 1234);
    TEST_CHECK(diskspace_hfs(&d, 0, &s) && s.free == 1234 * 4096 && s.size == count * 4096ull);
    put16(m + 126, start + 1);                  /* the extent pointing elsewhere */
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    free(im.data);
}

/* An Apple partition map entry */
static void map_entry(uint8_t *disk, int i, uint32_t entries, uint32_t start, uint32_t blocks, const char *name,
                      const char *type)
{
    uint8_t *e = disk + i * 512;
    e[0] = 'P';
    e[1] = 'M';
    put32(e + 4, entries);
    put32(e + 8, start);
    put32(e + 12, blocks);
    strcpy((char *)e + 16, name);
    strcpy((char *)e + 48, type);
}

static void test_partition_map(void)
{
    diskspace_t s;
    /* Two HFS partitions (800K and 2 MB), a driver and free space between */
    uint32_t a = 64, a_blocks = 1600, b = 2000, b_blocks = 4096, total = 8192;
    test_image_t im = image(total * 512ull);
    diskspace_disk_t d = test_disk(&im);
    im.data[0] = 'E';
    im.data[1] = 'R';
    put16(im.data + 2, 512);
    put32(im.data + 4, total);
    map_entry(im.data, 1, 5, 1, 63, "Apple", "Apple_partition_map");
    map_entry(im.data, 2, 5, a, a_blocks, "First", "Apple_HFS");
    map_entry(im.data, 3, 5, a + a_blocks, b - a - a_blocks, "Extra", "Apple_Free");
    map_entry(im.data, 4, 5, b, b_blocks, "Second", "Apple_HFS");
    map_entry(im.data, 5, 5, b + b_blocks, 100, "Driver", "Apple_Driver43");
    mdb(im.data + a * 512, 0x4244, 1594, 512, 6, 500);
    mdb(im.data + b * 512, 0x4244, 4080, 512, 8, 4000);
    TEST_CHECK(diskspace_mac_disk(&d, &s) && s.free == (500 + 4000) * 512 && s.size == (1594 + 4080) * 512);
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    /* the one the Mac uses, from its data offset */
    TEST_CHECK(diskspace_hfs(&d, a * 512, &s) && s.free == 500 * 512);
    /* an unformatted partition is left out */
    memset(im.data + b * 512 + 1024, 0, 512);
    TEST_CHECK(diskspace_mac_disk(&d, &s) && s.free == 500 * 512 && s.size == 1594 * 512);
    /* none formatted: no answer */
    memset(im.data + a * 512 + 1024, 0, 512);
    TEST_CHECK(!diskspace_mac_disk(&d, &s));
    /* HFS+ in an Apple_HFS partition */
    hfs_plus(im.data + b * 512, 0x482B, 4096, 512, 77);
    TEST_CHECK(diskspace_mac_disk(&d, &s) && s.free == 77 * 4096 && s.size == 512 * 4096);
    free(im.data);
}

/* A ProDOS volume of total blocks in ProDOS order: the volume directory
 * in blocks 2-5, the bitmap from block 6, blocks 0 to used - 1 in use
 */
static void prodos(test_image_t *im, uint32_t total, uint32_t used)
{
    uint8_t *v = im->data + 2 * 512;
    le16(v + 2, 3);
    v[4] = 0xF0 | 4;
    memcpy(v + 5, "TEST", 4);
    v[0x23] = 0x27;
    v[0x24] = 0x0D;
    le16(v + 0x27, 6);
    le16(v + 0x29, total);
    uint8_t *bm = im->data + 6 * 512;
    for (uint32_t n = used; n < total; n++) {
        bm[n / 8] |= 0x80 >> (n % 8);
    }
}

static void test_prodos(void)
{
    diskspace_t s;
    /* 140K: 280 blocks, 7 used */
    test_image_t im = image(143360);
    diskspace_disk_t d = test_disk(&im);
    prodos(&im, 280, 7);
    TEST_CHECK(diskspace_prodos(&d, 0, &s) && s.free == 273 * 512 && s.size == 143360);
    TEST_CHECK(!diskspace_dos33(&d, &s) && !diskspace_hfs(&d, 0, &s));
    /* a used block in the middle */
    im.data[6 * 512 + 100 / 8] &= ~(0x80 >> (100 % 8));
    TEST_CHECK(diskspace_prodos(&d, 0, &s) && s.free == 272 * 512);
    /* after a 64-byte 2IMG header */
    test_image_t im2 = image(143360 + 64);
    diskspace_disk_t d2 = test_disk(&im2);
    test_image_t inner = { im2.data + 64, 143360 };
    prodos(&inner, 280, 10);
    TEST_CHECK(diskspace_prodos(&d2, 64, &s) && s.free == 270 * 512 && s.size == 143360);
    TEST_CHECK(!diskspace_prodos(&d2, 0, &s));
    free(im2.data);
    /* insane: bigger than the image, no name, wrong entry length */
    le16(im.data + 2 * 512 + 0x29, 281);
    TEST_CHECK(!diskspace_prodos(&d, 0, &s));
    le16(im.data + 2 * 512 + 0x29, 280);
    im.data[2 * 512 + 4] = 0xF0;
    TEST_CHECK(!diskspace_prodos(&d, 0, &s));
    im.data[2 * 512 + 4] = 0xF4;
    im.data[2 * 512 + 0x23] = 0x28;
    TEST_CHECK(!diskspace_prodos(&d, 0, &s));
    im.data[2 * 512 + 0x23] = 0x27;
    im.data[2 * 512 + 4] = 0xE4;                /* a subdirectory's header */
    TEST_CHECK(!diskspace_prodos(&d, 0, &s));
    free(im.data);

    /* 1003 blocks, bits past the end set: not counted */
    im = image(1003 * 512);
    d = test_disk(&im);
    prodos(&im, 1003, 7);
    im.data[6 * 512 + 1003 / 8] |= 0xFF;
    TEST_CHECK(diskspace_prodos(&d, 0, &s) && s.free == 996 * 512 && s.size == 1003 * 512);
    free(im.data);

    /* 32 MB: 65535 blocks, a bitmap of 16 blocks */
    im = image(65535 * 512ull);
    d = test_disk(&im);
    prodos(&im, 65535, 22);
    TEST_CHECK(diskspace_prodos(&d, 0, &s) && s.free == (65535 - 22) * 512ull && s.size == 65535 * 512ull);
    im.data[(6 + 15) * 512 + 100] = 0;         /* 8 used in the last bitmap block */
    TEST_CHECK(diskspace_prodos(&d, 0, &s) && s.free == (65535 - 30) * 512ull);
    free(im.data);
}

/* A freshly initialised DOS 3.3 disk: tracks 0-2 (DOS) and 17 (VTOC and
 * catalog) in use, 496 sectors free
 */
static void dos33(test_image_t *im)
{
    uint8_t *v = im->data + 17 * 16 * 256;
    v[0x01] = 17;
    v[0x02] = 15;
    v[0x03] = 3;
    v[0x06] = 254;
    v[0x27] = 122;
    v[0x34] = 35;
    v[0x35] = 16;
    le16(v + 0x36, 256);
    for (int t = 0; t < 35; t++) {
        uint8_t *b = v + 0x38 + 4 * t;
        if (t > 2 && t != 17) {
            b[0] = b[1] = 0xFF;
        }
        b[2] = b[3] = 0xFF;                     /* (not used: left out) */
    }
}

static void test_dos33(void)
{
    diskspace_t s;
    test_image_t im = image(143360);
    diskspace_disk_t d = test_disk(&im);
    dos33(&im);
    TEST_CHECK(diskspace_dos33(&d, &s) && s.free == 496 * 256 && s.size == 143360);
    TEST_CHECK(!diskspace_prodos(&d, 0, &s) && !diskspace_hfs(&d, 0, &s));
    uint8_t *v = im.data + 17 * 16 * 256;
    v[0x38 + 4 * 20] = 0x0F;                    /* track 20: sectors 15-12 used */
    TEST_CHECK(diskspace_dos33(&d, &s) && s.free == 492 * 256);
    v[0x34] = 40;                               /* 40 tracks: not in a 140K image */
    TEST_CHECK(!diskspace_dos33(&d, &s));
    d.size = 0;                                 /* (size not known) */
    TEST_CHECK(diskspace_dos33(&d, &s) && s.size == 40 * 16 * 256);
    d.size = im.size;
    v[0x34] = 35;
    v[0x03] = 2;
    TEST_CHECK(!diskspace_dos33(&d, &s));
    v[0x03] = 3;
    v[0x35] = 13;
    TEST_CHECK(!diskspace_dos33(&d, &s));
    v[0x35] = 16;
    v[0x01] = 0;
    TEST_CHECK(!diskspace_dos33(&d, &s));
    free(im.data);
    /* too small for a VTOC */
    im = image(17 * 16 * 256);
    d = test_disk(&im);
    TEST_CHECK(!diskspace_dos33(&d, &s));
    free(im.data);
}

static void try_file(const char *path)
{
    test_image_t im = test_load(path);
    diskspace_disk_t d = test_disk(&im);
    diskspace_t s;
    printf("%s (%llu bytes):\n", path, (unsigned long long)im.size);
    if (diskspace_mac_disk(&d, &s)) {
        printf("  Mac disk: %llu bytes free of %llu\n", (unsigned long long)s.free, (unsigned long long)s.size);
    }
    if (diskspace_hfs(&d, 0, &s)) {
        printf("  HFS: %llu bytes free of %llu\n", (unsigned long long)s.free, (unsigned long long)s.size);
    }
    if (diskspace_prodos(&d, 0, &s)) {
        printf("  ProDOS: %llu bytes free of %llu\n", (unsigned long long)s.free, (unsigned long long)s.size);
    }
    if (im.size > 64 && memcmp(im.data, "2IMG", 4) == 0 && diskspace_prodos(&d, 64, &s)) {
        printf("  ProDOS (2IMG): %llu bytes free of %llu\n", (unsigned long long)s.free,
               (unsigned long long)s.size);
    }
    if (diskspace_dos33(&d, &s)) {
        printf("  DOS 3.3: %llu bytes free of %llu\n", (unsigned long long)s.free, (unsigned long long)s.size);
    }
    free(im.data);
}

int main(int argc, char **argv)
{
    test_mfs_hfs();
    test_hfs_plus();
    test_partition_map();
    test_prodos();
    test_dos33();
    for (int i = 1; i < argc; i++) {
        try_file(argv[i]);
    }
    printf("apple: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
