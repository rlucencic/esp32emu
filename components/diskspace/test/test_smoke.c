/* Every counter refuses an image of zeros */
#include "test.h"

int main(void)
{
    static uint8_t zeros[2 * 1024 * 1024];
    test_image_t im = { zeros, sizeof(zeros) };
    diskspace_disk_t d = test_disk(&im);
    diskspace_t s;
    diskspace_cpm_geometry_t g = { 512, 9, 40, 0, 1024, 64 };
    TEST_CHECK(!diskspace_fat(&d, 0, &s));
    TEST_CHECK(!diskspace_fat_disk(&d, &s));
    TEST_CHECK(!diskspace_hfs(&d, 0, &s));
    TEST_CHECK(!diskspace_mac_disk(&d, &s));
    TEST_CHECK(!diskspace_prodos(&d, 0, &s));
    TEST_CHECK(!diskspace_dos33(&d, &s));
    TEST_CHECK(!diskspace_cpm(&d, &g, &s) || s.free <= s.size);
    TEST_CHECK(!diskspace_trdos(&d, &s));
    TEST_CHECK(!diskspace_cbm(&d, &s));
    TEST_CHECK(!diskspace_atari(&d, 128, &s));
    TEST_CHECK(!diskspace_rsdos(&d, &s));
    TEST_CHECK(!diskspace_os9(&d, &s));
    TEST_CHECK(!diskspace_adfs(&d, &s));
    printf("smoke: %s\n", test_failures ? "FAILED" : "ok");
    return test_failures != 0;
}
