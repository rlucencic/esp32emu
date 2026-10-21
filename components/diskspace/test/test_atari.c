/* Atari 8-bit: DOS 2.0S, 2.0D, 2.5 enhanced density, MyDOS and SpartaDOS
 * disks made here as their DOSes format them; build/test_atari FILE...
 * prints the figures of .ATR images (or raw .XFD)
 */
#include "test.h"

static uint8_t *sector(uint8_t *img, uint32_t n, uint32_t sector_size)
{
    return img + (sector_size == 128 || n <= 3 ? (n - 1) * 128 : (n - 1) * sector_size - 384);
}

static uint32_t image_size(uint32_t sectors, uint32_t sector_size)
{
    return sector_size == 128 ? sectors * 128 : 384 + (sectors - 3) * sector_size;
}

static void set_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void set_free(uint8_t *bitmap, uint32_t n)
{
    bitmap[n / 8] |= (uint8_t)(0x80 >> (n % 8));
}

/* DOS 2's format: the VTOC (bitmap of sectors 0-719 from byte 10: boot
 * sectors 1-3, the VTOC and the directory 360-368, and 720 in use) and an
 * empty directory; ed: DOS 2.5's second VTOC in sector 1024
 */
static void format_dos2(uint8_t *img, uint32_t sector_size, bool ed)
{
    uint8_t *v = sector(img, 360, sector_size);
    v[0] = 2;
    uint32_t n_free = 0;
    for (uint32_t n = 4; n < 720; n++) {
        if (n < 360 || n > 368) {
            set_free(v + 10, n);
            n_free++;
        }
    }
    set_le16(v + 1, ed ? 1010 : n_free);
    set_le16(v + 3, n_free);
    if (ed) {
        uint8_t *v2 = sector(img, 1024, 128);
        uint32_t more = 0;
        memcpy(v2, v + 10 + 6, 84);                 /* sectors 48-719 again */
        for (uint32_t n = 721; n < 1024; n++) {
            set_free(v2 + 84, n - 720);
            more++;
        }
        set_le16(v2 + 122, more);
    }
}

/* SpartaDOS's boot sector (the figures it keeps there) */
static void format_sparta(uint8_t *img, uint32_t sector_size, uint32_t total, uint32_t free_sectors)
{
    uint8_t *b = sector(img, 1, sector_size);
    b[0] = 0;
    b[1] = 3;
    b[2] = 0x00;
    b[3] = 0x30;
    b[6] = 0x4C;
    b[7] = 0x80;
    b[8] = 0x30;
    set_le16(b + 9, 5);
    set_le16(b + 11, total);
    set_le16(b + 13, free_sectors);
    b[15] = 1;
    set_le16(b + 16, 4);
    memcpy(b + 22, "BLANK   ", 8);
    b[30] = 40;
    b[31] = sector_size == 128 ? 0x80 : 0x00;
    b[32] = 0x20;
}

static bool space(uint8_t *data, uint64_t size, uint32_t sector_size, diskspace_t *s)
{
    test_image_t im = { data, size };
    diskspace_disk_t d = test_disk(&im);
    return diskspace_atari(&d, sector_size, s);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        test_image_t im = test_load(argv[i]);
        uint8_t *data = im.data;
        uint64_t size = im.size;
        uint32_t sector_size = size == 183936 ? 256 : 128;
        if (size >= 16 && data[0] == 0x96 && data[1] == 0x02) {
            sector_size = data[4] | data[5] << 8;
            data += 16;
            size -= 16;
        }
        if (sector_size == 256 && size % 256 == 0 && size >= 768) {
            /* boot sectors in 768 bytes (each in a slot, or packed): packed
             * as the counter reads them
             */
            bool padded = true;
            for (int k = 0; k < 3; k++) {
                for (int j = 128; j < 256; j++) {
                    padded = padded && data[k * 256 + j] == 0;
                }
            }
            for (int k = 1; k < 3; k++) {
                memmove(data + k * 128, data + k * (padded ? 256 : 128), 128);
            }
            memmove(data + 384, data + 768, size - 768);
            size -= 384;
        }
        diskspace_t s;
        if (space(data, size, sector_size, &s)) {
            printf("%s: %llu sectors of %u free of %llu\n", argv[i], (unsigned long long)s.free / sector_size,
                   sector_size, (unsigned long long)s.size / sector_size);
        } else {
            printf("%s: no DOS 2, MyDOS or SpartaDOS figures\n", argv[i]);
        }
        free(im.data);
    }
    if (argc > 1) {
        return 0;
    }

    static uint8_t img[65535 * 256];
    diskspace_t s;
    uint32_t sd = image_size(720, 128), ed = image_size(1040, 128), dd = image_size(720, 256);

    /* blank (zeros, as made for DOS to format) */
    memset(img, 0, sizeof(img));
    TEST_CHECK(!space(img, sd, 128, &s));
    TEST_CHECK(!space(img, dd, 256, &s));

    /* DOS 2.0S: 707 FREE SECTORS; DOS.SYS (39) and DUP.SYS (42) written */
    format_dos2(img, 128, false);
    TEST_CHECK(space(img, sd, 128, &s) && s.free == 707 * 128 && s.size == 707 * 128);
    set_le16(sector(img, 360, 128) + 3, 707 - 81);
    TEST_CHECK(space(img, sd, 128, &s) && s.free == 626 * 128 && s.size == 707 * 128);
    TEST_CHECK(!space(img, 300 * 128, 128, &s));    /* too short for its VTOC */
    set_le16(sector(img, 360, 128) + 3, 708);       /* more free than usable */
    TEST_CHECK(!space(img, sd, 128, &s));
    sector(img, 360, 128)[0] = 0;
    set_le16(sector(img, 360, 128) + 3, 707);
    TEST_CHECK(!space(img, sd, 128, &s));

    /* DOS 2.5 enhanced density: 1010 FREE SECTORS, some used up high */
    memset(img, 0, sizeof(img));
    format_dos2(img, 128, true);
    TEST_CHECK(space(img, ed, 128, &s) && s.free == 1010 * 128 && s.size == 1010 * 128);
    set_le16(sector(img, 1024, 128) + 122, 303 - 10);
    set_le16(sector(img, 360, 128) + 3, 707 - 81);
    TEST_CHECK(space(img, ed, 128, &s) && s.free == (1010 - 91) * 128);

    /* DOS 2.0D (256-byte sectors, the first three 128) */
    memset(img, 0, sizeof(img));
    format_dos2(img, 256, false);
    TEST_CHECK(space(img, dd, 256, &s) && s.free == 707 * 256 && s.size == 707 * 256);
    TEST_CHECK(!space(img, dd, 128, &s));           /* read as 128-byte sectors: not there */

    /* MyDOS on a big disk: its own DOS code, longer VTOC */
    memset(img, 0, sizeof(img));
    uint32_t big = image_size(65535, 256);
    uint8_t *v = sector(img, 360, 256);
    v[0] = 2 + 33;
    set_le16(v + 1, 65535 - 3 - 33 - 8);
    set_le16(v + 3, 60000);
    TEST_CHECK(space(img, big, 256, &s) && s.free == 60000ull * 256 && s.size == (65535ull - 44) * 256);
    TEST_CHECK(!space(img, dd, 256, &s));           /* more sectors than the image has */

    /* SpartaDOS: single and double density */
    memset(img, 0, sizeof(img));
    format_sparta(img, 128, 720, 690);
    TEST_CHECK(space(img, sd, 128, &s) && s.free == 690 * 128 && s.size == 720 * 128);
    TEST_CHECK(!space(img, dd, 256, &s));           /* its sector size says 128 */
    memset(img, 0, sizeof(img));
    format_sparta(img, 256, 720, 700);
    TEST_CHECK(space(img, dd, 256, &s) && s.free == 700 * 256 && s.size == 720 * 256);
    sector(img, 1, 256)[32] = 0x99;                 /* unknown version */
    TEST_CHECK(!space(img, dd, 256, &s));
    sector(img, 1, 256)[32] = 0x21;
    set_le16(sector(img, 1, 256) + 11, 1040);       /* bigger than the image */
    TEST_CHECK(!space(img, dd, 256, &s));

    TEST_CHECK(!space(img, dd, 512, &s));
    printf("atari: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
