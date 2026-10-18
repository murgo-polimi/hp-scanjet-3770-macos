#ifndef HP3770_PROFILE_H
#define HP3770_PROFILE_H

#include <stdint.h>

struct hp3770_scan_profile {
    unsigned int dpi;
    unsigned int sensor_resolution;
    uint8_t sample_rate;
    uint8_t system_clock;
    uint8_t motor_step_type;
    uint16_t ctpc;
    uint8_t timing;
    uint8_t motor_curve;
    uint16_t motor_backstep;
    uint8_t dummy_lines;
};

/*
 * Returns the verified CCD profile 1 USB-2 grayscale plan at a supported
 * native DPI. The attached ScanJet 3770 selected this branch at 0xf910.
 */
const struct hp3770_scan_profile *hp3770_usb2_gray_profile(unsigned int dpi);

#endif
