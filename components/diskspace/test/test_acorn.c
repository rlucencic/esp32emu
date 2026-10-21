/* ADFS: old map S, M, L, D discs, new map E (esp32arc's blank disc), F (4
 * zones, with and without a boot block), a 20 MB IDE disc (esp32arc's
 * blank, as HForm 2.19 makes it); build/test_acorn FILE... prints files'
 * figures
 */
#include "test.h"

static void put24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

static void put32(uint8_t *p, uint32_t v)
{
    put24(p, v);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- Old map ---- */

/* As the ADFS documentation gives it: bytes 254 to 0 added with carry,
 * starting with 255
 */
static uint8_t old_check(const uint8_t *s)
{
    int sum = 255;
    for (int i = 254; i >= 0; i--) {
        if (sum > 255) {
            sum = (sum & 255) + 1;
        }
        sum += s[i];
    }
    return (uint8_t)(sum & 255);
}

/* sectors of 256 bytes, the root directory at root, free areas (start, length) */
static test_image_t old_disc(uint32_t sectors, uint32_t root, const uint32_t areas[][2], int n)
{
    test_image_t im = { calloc(sectors, 256), (uint64_t)sectors * 256 };
    uint8_t *m = im.data;
    for (int i = 0; i < n; i++) {
        put24(m + 3 * i, areas[i][0]);
        put24(m + 256 + 3 * i, areas[i][1]);
    }
    put24(m + 0xFC, sectors);
    memcpy(m + 0xF7, "Test ", 5);
    m[0x1FE] = (uint8_t)(3 * n);
    memcpy(m + root + 1, "Hugo", 4);
    m[0xFF] = old_check(m);
    m[0x1FF] = old_check(m + 256);
    return im;
}

static void check(test_image_t *im, uint64_t free_bytes, uint64_t size)
{
    diskspace_disk_t d = test_disk(im);
    diskspace_t s = { 0, 0 };
    bool ok = diskspace_adfs(&d, &s);
    TEST_CHECK(ok);
    TEST_CHECK(s.free == free_bytes);
    TEST_CHECK(s.size == size);
    if (!ok || s.free != free_bytes || s.size != size) {
        fprintf(stderr, "  got %d, %llu free of %llu; expected %llu of %llu\n", ok, (unsigned long long)s.free,
                (unsigned long long)s.size, (unsigned long long)free_bytes, (unsigned long long)size);
    }
}

static bool refused(test_image_t *im)
{
    diskspace_disk_t d = test_disk(im);
    diskspace_t s;
    return !diskspace_adfs(&d, &s);
}

static void old_maps(void)
{
    /* S: 640 sectors, the root in 2-6, all the rest free */
    static const uint32_t s[][2] = { { 7, 633 } };
    test_image_t im = old_disc(640, 0x200, s, 1);
    check(&im, 633 * 256, 163840);
    /* checks wrong: refused */
    im.data[0xFF] ^= 1;
    TEST_CHECK(refused(&im));
    im.data[0xFF] ^= 1;
    im.data[0x1FF] ^= 0x80;
    TEST_CHECK(refused(&im));
    free(im.data);

    /* M: 1280, two areas */
    static const uint32_t m[][2] = { { 20, 100 }, { 300, 980 } };
    im = old_disc(1280, 0x200, m, 2);
    check(&im, 1080 * 256, 327680);
    free(im.data);

    /* L: 2560, three areas */
    static const uint32_t l[][2] = { { 7, 1 }, { 1000, 60 }, { 2000, 560 } };
    im = old_disc(2560, 0x200, l, 3);
    check(&im, 621 * 256, 655360);
    /* no root directory there: refused */
    im.data[0x201] = 'h';
    TEST_CHECK(refused(&im));
    free(im.data);

    /* D: 800 KB of 1024-byte sectors, the size in 256-byte units, the root at &400 */
    static const uint32_t dd[][2] = { { 0xA90, 0x1F0 } };
    im = old_disc(3200, 0x400, dd, 1);
    check(&im, 0x1F0 * 256, 819200);
    free(im.data);

    /* full: no areas */
    im = old_disc(2560, 0x200, NULL, 0);
    check(&im, 0, 655360);
    free(im.data);

    /* an area past the disc's end: refused */
    static const uint32_t bad[][2] = { { 600, 41 } };
    im = old_disc(640, 0x200, bad, 1);
    TEST_CHECK(refused(&im));
    free(im.data);
}

/* ---- New map ---- */

/* esp32arc's floppy.c: a zone's check byte */
static uint8_t zone_check(const uint8_t *zone, uint32_t size)
{
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uint32_t i = size - 4;
    for (; i > 0; i -= 4) {
        s0 += zone[i] + (s3 >> 8);
        s3 &= 0xFF;
        s1 += zone[i + 1] + (s0 >> 8);
        s0 &= 0xFF;
        s2 += zone[i + 2] + (s1 >> 8);
        s1 &= 0xFF;
        s3 += zone[i + 3] + (s2 >> 8);
        s2 &= 0xFF;
    }
    s0 += s3 >> 8;
    s1 += zone[1] + (s0 >> 8);
    s2 += zone[2] + (s1 >> 8);
    s3 += zone[3] + (s2 >> 8);
    return (uint8_t)(s0 ^ s1 ^ s2 ^ s3);
}

typedef struct {
    uint16_t at;
    uint8_t value;
} image_byte_t;

/* esp32arc's floppy.c: an empty E disc as RISC OS 3.11's *Format makes it */
static const image_byte_t s_e_map[] = {
    { 0x001, 0x18 }, { 0x002, 0x82 }, { 0x003, 0xFF }, { 0x004, 0x0A }, { 0x005, 0x05 }, { 0x006, 0x02 },
    { 0x007, 0x02 }, { 0x008, 0x0F }, { 0x009, 0x07 }, { 0x00A, 0x01 }, { 0x00D, 0x01 }, { 0x00E, 0x20 },
    { 0x00F, 0x05 }, { 0x010, 0x03 }, { 0x011, 0x02 }, { 0x015, 0x80 }, { 0x016, 0x0C }, { 0x01A, 0x42 },
    { 0x01B, 0x6C }, { 0x01C, 0x61 }, { 0x01D, 0x6E }, { 0x01E, 0x6B }, { 0x01F, 0x20 }, { 0x020, 0x20 },
    { 0x021, 0x20 }, { 0x022, 0x20 }, { 0x023, 0x20 }, { 0x040, 0x02 }, { 0x043, 0x80 }, { 0x35F, 0x80 },
};

static const image_byte_t s_e_root[] = {
    { 0x001, 0x4E }, { 0x002, 0x69 }, { 0x003, 0x63 }, { 0x004, 0x6B }, { 0x7DA, 0x03 }, { 0x7DB, 0x02 },
    { 0x7DD, 0x42 }, { 0x7DE, 0x6C }, { 0x7DF, 0x61 }, { 0x7E0, 0x6E }, { 0x7E1, 0x6B }, { 0x7E2, 0x0D },
    { 0x7F0, 0x42 }, { 0x7F1, 0x6C }, { 0x7F2, 0x61 }, { 0x7F3, 0x6E }, { 0x7F4, 0x6B }, { 0x7F5, 0x0D },
    { 0x7FB, 0x4E }, { 0x7FC, 0x69 }, { 0x7FD, 0x63 }, { 0x7FE, 0x6B }, { 0x7FF, 0xB7 },
};

static test_image_t e_blank(void)
{
    test_image_t im = { calloc(819200, 1), 819200 };
    for (int copy = 0; copy < 2; copy++) {
        uint8_t *z = im.data + copy * 1024;
        for (size_t i = 0; i < sizeof(s_e_map) / sizeof(s_e_map[0]); i++) {
            z[s_e_map[i].at] = s_e_map[i].value;
        }
        z[0x18] = 0x34;                 /* a disc ID */
        z[0x19] = 0x12;
        z[0] = zone_check(z, 1024);
    }
    for (size_t i = 0; i < sizeof(s_e_root) / sizeof(s_e_root[0]); i++) {
        im.data[0x800 + s_e_root[i].at] = s_e_root[i].value;
    }
    return im;
}

static void set_bit(uint8_t *z, uint32_t n, bool v)
{
    if (v) {
        z[n >> 3] |= (uint8_t)(1u << (n & 7));
    } else {
        z[n >> 3] &= (uint8_t)~(1u << (n & 7));
    }
}

/* A fragment of len bits at bit at: id in idlen bits, zeros, a 1 */
static void fragment(uint8_t *z, uint32_t at, uint32_t len, uint32_t id, uint32_t idlen)
{
    for (uint32_t i = 0; i < len; i++) {
        set_bit(z, at + i, i < idlen ? (id >> i) & 1 : i == len - 1);
    }
}

static void free_link(uint8_t *z, uint32_t first)
{
    uint32_t v = first ? (first - 8) | 0x8000 : 0x8000;
    z[1] = (uint8_t)v;
    z[2] = (uint8_t)(v >> 8);
}

static void e_maps(void)
{
    /* blank: 6368 bits of 128 bytes free (the maps and the root take 32) */
    test_image_t im = e_blank();
    check(&im, 815104, 819200);
    /* the first copy's check byte wrong: the second copy */
    im.data[0] ^= 0x55;
    check(&im, 815104, 819200);
    im.data[0x400] ^= 0x55;
    TEST_CHECK(refused(&im));
    free(im.data);
    /* a zone spare bigger than the zone: refused */
    im = e_blank();
    for (int copy = 0; copy < 2; copy++) {
        im.data[copy * 1024 + 0x0E] = 0xFF;
        im.data[copy * 1024 + 0x0F] = 0xFF;
        im.data[copy * 1024] = zone_check(im.data + copy * 1024, 1024);
    }
    TEST_CHECK(refused(&im));
    free(im.data);

    /* a file of 100 bits after the root, the rest free */
    im = e_blank();
    uint8_t *z = im.data;
    fragment(z, 544, 100, 3, 15);
    fragment(z, 644, 6912 - 644, 0, 15);
    free_link(z, 644);
    z[0] = zone_check(z, 1024);
    memcpy(im.data + 1024, z, 1024);
    check(&im, (6912 - 644) * 128, 819200);
    /* two free fragments, linked */
    fragment(z, 644, 1000, 2000, 15);   /* to the next free one, 2000 bits on */
    fragment(z, 1644, 1000, 7, 15);
    fragment(z, 2644, 6912 - 2644, 0, 15);
    z[0] = zone_check(z, 1024);
    memcpy(im.data + 1024, z, 1024);
    check(&im, (1000 + 6912 - 2644) * 128, 819200);
    /* a link into the middle of a fragment: refused */
    fragment(z, 644, 1000, 500, 15);
    z[0] = zone_check(z, 1024);
    memcpy(im.data + 1024, z, 1024);
    TEST_CHECK(refused(&im));
    /* no free space */
    fragment(z, 644, 6912 - 644, 4, 15);
    free_link(z, 0);
    z[0] = zone_check(z, 1024);
    memcpy(im.data + 1024, z, 1024);
    check(&im, 0, 819200);
    /* shorter images (the last tracks left out) are read the same */
    im.size = 409600;
    check(&im, 0, 819200);
    free(im.data);
}

/* F: 4 zones of 1024 bytes (6592 bits each), 64 bytes a bit, the map at
 * the middle zone's start (bit 12704: &C6800)
 */
static test_image_t f_disc(bool boot_block, bool past_end_free)
{
    test_image_t im = { calloc(1638400, 1), 1638400 };
    uint8_t dr[60] = { 10, 10, 2, 4, 15, 6, 1, 0, 0, 4 };
    dr[10] = 1600 & 0xFF;
    dr[11] = 1600 >> 8;
    put32(dr + 12, 0x33801);
    put32(dr + 16, 1638400);
    memcpy(dr + 22, "Test F    ", 10);
    uint8_t *map = im.data + 0xC6800;
    for (int zone = 0; zone < 4; zone++) {
        uint8_t *z = map + zone * 1024;
        uint32_t end = 32 + 6592, first;
        switch (zone) {
        case 0:                         /* the boot block's 4 KB, then free */
            memcpy(z + 4, dr, 60);
            fragment(z, 512, 64, 2, 15);
            first = 576;
            fragment(z, first, end - first, 0, 15);
            break;
        case 2:                         /* the map's two copies and the root, then free */
            fragment(z, 32, 160, 2, 15);
            first = 192;
            fragment(z, first, end - first, 0, 15);
            break;
        case 3:                         /* free to the disc's end at 6336, the rest not there */
            first = 32;
            if (past_end_free) {
                fragment(z, first, end - first, 0, 15);
            } else {
                fragment(z, first, 6336 - first, 0, 15);
                fragment(z, 6336, end - 6336, 1, 15);
            }
            break;
        default:
            first = 32;
            fragment(z, first, end - first, 0, 15);
            break;
        }
        free_link(z, first);
        z[3] = zone == 3 ? 0xFF : (uint8_t)(0x11 * zone);
        if (zone == 3) {
            z[3] ^= 0x11 ^ 0x22;        /* the cross check: all four XOR to &FF */
        }
        z[0] = zone_check(z, 1024);
    }
    memcpy(map + 4096, map, 4096);
    if (boot_block) {
        uint8_t *b = im.data + 0xC00;
        memcpy(b + 0x1C0, dr, 60);
        uint32_t sum = 0;
        for (int i = 510; i >= 0; i--) {
            sum = (sum & 0xFF) + (sum >> 8) + b[i];
        }
        b[511] = (uint8_t)sum;
    }
    return im;
}

static void f_maps(void)
{
    uint64_t free_bits = (6624 - 576) + 6592 + (6624 - 192) + (6336 - 32);
    test_image_t im = f_disc(true, false);
    check(&im, free_bits * 64, 1638400);
    free(im.data);
    im = f_disc(false, false);
    check(&im, free_bits * 64, 1638400);
    /* a free fragment past the disc's end counts to the end */
    free(im.data);
    im = f_disc(true, true);
    check(&im, free_bits * 64, 1638400);
    /* the cross check wrong in both copies: refused */
    for (int copy = 0; copy < 2; copy++) {
        uint8_t *z = im.data + 0xC6800 + copy * 4096;
        z[3] ^= 1;
        z[0] = zone_check(z, 1024);
    }
    TEST_CHECK(refused(&im));
    free(im.data);
}

/* ---- esp32arc's hd_blank.c: a 20 MB IDE disc (40 cylinders of 16 heads
 * of 63 sectors) as HForm 2.19 leaves it ---- */

typedef struct {
    uint32_t at;
    uint16_t len, data;
} run_t;

static const uint8_t s_data_40[] = {
    0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x20, 0x33, 0x01, 0x09, 0x3F, 0x10, 0x00, 0x0C, 0x09,
    0x00, 0x00, 0x01, 0x0B, 0x73, 0x01, 0x17, 0x02, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x01, 0xB8, 0x5E,
    0x05, 0x82, 0xFF, 0x09, 0x3F, 0x10, 0x00, 0x0C, 0x09, 0x00, 0x00, 0x01, 0x0B, 0x73, 0x01, 0x17,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x01, 0xC8, 0xAD, 0x49, 0x44, 0x45, 0x44, 0x69, 0x73, 0x63,
    0x34, 0x0D, 0x00, 0x00, 0x00, 0x04, 0x02, 0x10, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48,
    0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x62, 0x32,
    0x80, 0x00, 0x02, 0x00, 0x00, 0x02, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80,
    0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x78, 0x18, 0x80, 0x60,
    0x80, 0x5E, 0x05, 0x82, 0xFF, 0x09, 0x3F, 0x10, 0x00, 0x0C, 0x09, 0x00, 0x00, 0x01, 0x0B, 0x73,
    0x01, 0x17, 0x02, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x01, 0xC8, 0xAD, 0x49, 0x44, 0x45, 0x44, 0x69,
    0x73, 0x63, 0x34, 0x0D, 0x00, 0x00, 0x00, 0x04, 0x02, 0x10, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30,
    0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80,
    0x62, 0x32, 0x80, 0x00, 0x02, 0x00, 0x00, 0x02, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48,
    0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x48, 0x18, 0x80, 0x30, 0x80, 0x78, 0x18,
    0x80, 0x60, 0x80, 0x00, 0x48, 0x75, 0x67, 0x6F, 0x17, 0x02, 0x00, 0x24, 0x24, 0x48, 0x75, 0x67,
    0x6F, 0x3D,
};

static const run_t s_runs_40[] = {
    { 0x0000C03, 1, 0 }, { 0x0000DAC, 4, 1 }, { 0x0000DBB, 25, 5 },
    { 0x0000DFF, 1, 30 }, { 0x08DC200, 39, 31 }, { 0x08DC240, 2, 70 },
    { 0x08DC3D5, 1, 72 }, { 0x08DC3FF, 4, 73 }, { 0x08DC5D5, 1, 77 },
    { 0x08DC5FF, 4, 78 }, { 0x08DC7D5, 1, 82 }, { 0x08DC7FF, 4, 83 },
    { 0x08DC9D5, 1, 87 }, { 0x08DC9FF, 4, 88 }, { 0x08DCBD5, 1, 92 },
    { 0x08DCBFF, 9, 93 }, { 0x08DCDD5, 1, 102 }, { 0x08DCDFF, 4, 103 },
    { 0x08DCFD5, 1, 107 }, { 0x08DCFFF, 4, 108 }, { 0x08DD1D5, 1, 112 },
    { 0x08DD1FF, 4, 113 }, { 0x08DD3D5, 1, 117 }, { 0x08DD3FF, 4, 118 },
    { 0x08DD5D5, 1, 122 }, { 0x08DD5FF, 4, 123 }, { 0x08DD7BF, 1, 127 },
    { 0x08DD7FF, 40, 128 }, { 0x08DD840, 2, 168 }, { 0x08DD9D5, 1, 170 },
    { 0x08DD9FF, 4, 171 }, { 0x08DDBD5, 1, 175 }, { 0x08DDBFF, 4, 176 },
    { 0x08DDDD5, 1, 180 }, { 0x08DDDFF, 4, 181 }, { 0x08DDFD5, 1, 185 },
    { 0x08DDFFF, 4, 186 }, { 0x08DE1D5, 1, 190 }, { 0x08DE1FF, 9, 191 },
    { 0x08DE3D5, 1, 200 }, { 0x08DE3FF, 4, 201 }, { 0x08DE5D5, 1, 205 },
    { 0x08DE5FF, 4, 206 }, { 0x08DE7D5, 1, 210 }, { 0x08DE7FF, 4, 211 },
    { 0x08DE9D5, 1, 215 }, { 0x08DE9FF, 4, 216 }, { 0x08DEBD5, 1, 220 },
    { 0x08DEBFF, 4, 221 }, { 0x08DEDBF, 1, 225 }, { 0x08DEDFF, 6, 226 },
    { 0x08DF5DA, 4, 232 }, { 0x08DF5F0, 1, 236 }, { 0x08DF5FB, 5, 237 },
};

static void hard_disc(void)
{
    test_image_t im = { calloc(40 * 516096, 1), 40 * 516096 };
    for (size_t i = 0; i < sizeof(s_runs_40) / sizeof(s_runs_40[0]); i++) {
        memcpy(im.data + s_runs_40[i].at, s_data_40 + s_runs_40[i].data, s_runs_40[i].len);
    }
    /* 11 zones of 3725 bits of 512 bytes; RISC OS 3.11's *Free on it: 20623872 */
    check(&im, 20623872, 20643840);
    /* the boot block's check byte wrong: refused (the map isn't at the start) */
    im.data[0xDFF] ^= 1;
    TEST_CHECK(refused(&im));
    free(im.data);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            test_image_t im = test_load(argv[i]);
            diskspace_disk_t d = test_disk(&im);
            diskspace_t s;
            if (diskspace_adfs(&d, &s)) {
                printf("%s: %llu bytes free of %llu\n", argv[i], (unsigned long long)s.free,
                       (unsigned long long)s.size);
            } else {
                printf("%s: no ADFS disc\n", argv[i]);
            }
            free(im.data);
        }
        return 0;
    }
    old_maps();
    e_maps();
    f_maps();
    hard_disc();
    printf("acorn: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
