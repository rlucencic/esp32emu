/* TR-DOS: blank disks of each type, files, what isn't TR-DOS. With files as
 * arguments (TRD images): their figures
 */
#include "test.h"

static uint8_t s_img[80 * 2 * 16 * 256];

/* A disk of `type` with its system sector, free_sectors left */
static test_image_t disk(uint8_t type, int free_sectors)
{
    test_image_t im = { s_img, sizeof(s_img) };
    memset(s_img, 0, sizeof(s_img));
    uint8_t *sys = s_img + 8 * 256;
    sys[0xE2] = 1;
    sys[0xE3] = type;
    sys[0xE5] = free_sectors & 0xFF;
    sys[0xE6] = free_sectors >> 8;
    sys[0xE7] = 0x10;
    memset(sys + 0xF5, ' ', 8);
    return im;
}

static bool count(test_image_t *im, diskspace_t *s)
{
    diskspace_disk_t d = test_disk(im);
    return diskspace_trdos(&d, s);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            test_image_t im = test_load(argv[i]);
            diskspace_t s;
            if (count(&im, &s)) {
                printf("%s: %llu bytes free of %llu\n", argv[i], (unsigned long long)s.free,
                       (unsigned long long)s.size);
            } else {
                printf("%s: not TR-DOS\n", argv[i]);
            }
            free(im.data);
        }
        return 0;
    }
    diskspace_t s;
    test_image_t im = disk(0x16, 2544);
    TEST_CHECK(count(&im, &s) && s.free == 2544 * 256 && s.size == 2544 * 256);
    im = disk(0x16, 2544 - 37);
    TEST_CHECK(count(&im, &s) && s.free == 2507 * 256 && s.size == 2544 * 256);
    im = disk(0x17, 1264);
    TEST_CHECK(count(&im, &s) && s.free == 1264 * 256 && s.size == 1264 * 256);
    im = disk(0x18, 1264);
    TEST_CHECK(count(&im, &s) && s.free == 1264 * 256 && s.size == 1264 * 256);
    im = disk(0x19, 600);
    TEST_CHECK(count(&im, &s) && s.free == 600 * 256 && s.size == 624 * 256);
    /* an image cut short after its last file */
    im = disk(0x16, 2000);
    im.size = 16 * 256;
    TEST_CHECK(count(&im, &s) && s.free == 2000 * 256 && s.size == 2544 * 256);
    /* not TR-DOS */
    im = disk(0x16, 2544);
    s_img[8 * 256 + 0xE7] = 0;
    TEST_CHECK(!count(&im, &s));
    im = disk(0x20, 100);
    TEST_CHECK(!count(&im, &s));
    im = disk(0x19, 625);
    TEST_CHECK(!count(&im, &s));
    im = disk(0x16, 100);
    im.size = 8 * 256 + 200;
    TEST_CHECK(!count(&im, &s));
    printf("trdos: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
