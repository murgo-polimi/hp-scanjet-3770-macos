/*
 * USB-2 reflective grayscale profiles recovered from the original HP 3770
 * backend's t_RtIniFile configuration (CCD profile 1). These are parameters
 * only: this file does not construct registers or send commands to a scanner.
 */
#include <stddef.h>

#include "hp3770_profile.h"

/*
 * sensor_resolution is the SensorResolution of the selected t_TimingN table:
 * Timing 10 is the 200 dpi table (100 dpi = 2:1 of it), Timing 9 = 300 dpi,
 * Timing 8 = 600, Timing 7 = 1200, Timing 6 = 2400. (Earlier versions listed
 * 600 for Timing 10 because t_Timing8 had been read in its place.)
 */
static const struct hp3770_scan_profile usb2_gray_profiles[] = {
    {100,  200, 1, 4, 1,  2255, 10, 5, 1856, 2},
    {200,  200, 1, 4, 1,  2255, 10, 5, 1856, 0},
    {300,  600, 1, 4, 1,  2751,  9, 4,  768, 0},
    {600,  600, 1, 4, 0,  5499,  8, 3,  256, 0},
    {1200, 1200, 1, 5, 0, 14667,  7, 0,  256, 0},
    {2400, 2400, 1, 4, 0, 24499,  6, 0,  256, 0},
};

const struct hp3770_scan_profile *hp3770_usb2_gray_profile(unsigned int dpi) {
    for (unsigned int i = 0;
         i < sizeof(usb2_gray_profiles) / sizeof(usb2_gray_profiles[0]); ++i) {
        if (usb2_gray_profiles[i].dpi == dpi) return &usb2_gray_profiles[i];
    }
    return NULL;
}
