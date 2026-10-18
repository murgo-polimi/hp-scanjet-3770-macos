/*
 * Read-only USB diagnostic foundation for HP ScanJet 3770 (03f0:2505).
 * Optional commands claim interface 0 only long enough to issue documented
 * vendor IN control reads. They never write to the scanner or bulk endpoints.
 */
#include <libusb.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hp3770_profile.h"

#define HP_VENDOR_ID  0x03f0
#define SJ3770_PRODUCT_ID 0x2505
#define RTS8822_REGISTER_BASE 0xe800
#define RTS8822_REGISTER_BYTES 0x071a

static void print_endpoint(const struct libusb_endpoint_descriptor *ep) {
    unsigned int direction = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) ? 1 : 0;
    printf("      endpoint: address=0x%02x direction=%s transfer_type=%u max_packet=%u interval=%u\n",
           ep->bEndpointAddress, direction ? "IN" : "OUT",
           ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK,
           ep->wMaxPacketSize, ep->bInterval);
}

static void print_configuration(const struct libusb_config_descriptor *config) {
    printf("  configuration: value=%u interfaces=%u attributes=0x%02x max_power=%umA\n",
           config->bConfigurationValue, config->bNumInterfaces,
           config->bmAttributes, config->MaxPower * 2);

    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int alt = 0; alt < iface->num_altsetting; ++alt) {
            const struct libusb_interface_descriptor *d = &iface->altsetting[alt];
            printf("    interface: number=%u alt=%u class=0x%02x subclass=0x%02x protocol=0x%02x endpoints=%u\n",
                   d->bInterfaceNumber, d->bAlternateSetting, d->bInterfaceClass,
                   d->bInterfaceSubClass, d->bInterfaceProtocol, d->bNumEndpoints);
            for (uint8_t ep = 0; ep < d->bNumEndpoints; ++ep) {
                print_endpoint(&d->endpoint[ep]);
            }
        }
    }
}

static void print_string_descriptor(libusb_device_handle *handle, uint8_t index,
                                    const char *label) {
    unsigned char value[256] = {0};
    if (index == 0) {
        printf("  %s: (not provided)\n", label);
        return;
    }
    int rc = libusb_get_string_descriptor_ascii(handle, index, value,
                                                 (int)sizeof(value) - 1);
    if (rc < 0) {
        printf("  %s: unavailable (%s)\n", label, libusb_error_name(rc));
        return;
    }
    printf("  %s: %s\n", label, value);
}

static void print_identity_strings(libusb_device *device,
                                   const struct libusb_device_descriptor *desc) {
    libusb_device_handle *handle = NULL;
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  identity strings: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    print_string_descriptor(handle, desc->iManufacturer, "manufacturer");
    print_string_descriptor(handle, desc->iProduct, "product");
    print_string_descriptor(handle, desc->iSerialNumber, "serial");
    libusb_close(handle);
}

static void test_interface_claim(libusb_device *device) {
    libusb_device_handle *handle = NULL;
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  interface claim: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  interface claim: failed (%s)\n", libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    puts("  interface claim: succeeded (no USB transfer sent)");
    rc = libusb_release_interface(handle, 0);
    printf("  interface release: %s\n",
           rc == LIBUSB_SUCCESS ? "succeeded" : libusb_error_name(rc));
    libusb_close(handle);
}

/*
 * Capability read used by the original HP Linux backend immediately after it
 * opens the device. It is an IN vendor control request only:
 *   bmRequestType=0xc0, bRequest=0, wValue=0xfe11, wIndex=0x0100, length=1.
 */
static void read_usb2_capability(libusb_device *device) {
    libusb_device_handle *handle = NULL;
    unsigned char capability = 0;
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  capability read: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  capability read: interface claim failed (%s)\n",
               libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        0x00, 0xfe11, 0x0100, &capability, 1, 1000);
    if (rc == 1) {
        printf("  USB 2 capability: 0x%02x (USB 2 flag: %s)\n", capability,
               (capability & 0x01) ? "set" : "not set");
    } else if (rc >= 0) {
        printf("  capability read: unexpected response length %d\n", rc);
    } else {
        printf("  capability read: failed (%s)\n", libusb_error_name(rc));
    }
    int release_rc = libusb_release_interface(handle, 0);
    if (release_rc != LIBUSB_SUCCESS) {
        printf("  capability release: %s\n", libusb_error_name(release_rc));
    }
    libusb_close(handle);
}

/*
 * The legacy driver's USBGetVideoLength() routine first reads status at
 * wValue=0xe812. Only if the high two status bits are non-zero does it read
 * a 24-bit pending-byte count at wValue=0xef16. Both are IN control reads.
 */
static void read_pending_video_length(libusb_device *device) {
    libusb_device_handle *handle = NULL;
    unsigned char status = 0;
    unsigned char length[4] = {0};
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  video status: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  video status: interface claim failed (%s)\n",
               libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        0x00, 0xe812, 0x0100, &status, 1, 1000);
    if (rc != 1) {
        printf("  video status: failed (%s)\n",
               rc < 0 ? libusb_error_name(rc) : "unexpected response length");
    } else if ((status >> 6) == 0) {
        printf("  video status: 0x%02x (no image data pending)\n", status);
    } else {
        rc = libusb_control_transfer(
            handle,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
            0x00, 0xef16, 0x0100, length, 4, 1000);
        if (rc == 4) {
            unsigned long count = (unsigned long)length[0] |
                                  ((unsigned long)length[1] << 8) |
                                  ((unsigned long)length[2] << 16);
            printf("  video status: 0x%02x (%lu image bytes pending)\n",
                   status, count);
        } else {
            printf("  video length: failed (%s)\n",
                   rc < 0 ? libusb_error_name(rc) : "unexpected response length");
        }
    }
    int release_rc = libusb_release_interface(handle, 0);
    if (release_rc != LIBUSB_SUCCESS) {
        printf("  video status release: %s\n", libusb_error_name(release_rc));
    }
    libusb_close(handle);
}

/* CdRegReadRevisionID() in the legacy driver: IN read of one byte at 0xfe3e. */
static void read_chip_revision(libusb_device *device) {
    libusb_device_handle *handle = NULL;
    unsigned char revision = 0;
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  chip revision: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  chip revision: interface claim failed (%s)\n",
               libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        0x00, 0xfe3e, 0x0100, &revision, 1, 1000);
    if (rc == 1) {
        printf("  RTS8822 revision: 0x%02x\n", revision);
    } else {
        printf("  chip revision: failed (%s)\n",
               rc < 0 ? libusb_error_name(rc) : "unexpected response length");
    }
    int release_rc = libusb_release_interface(handle, 0);
    if (release_rc != LIBUSB_SUCCESS) {
        printf("  chip revision release: %s\n", libusb_error_name(release_rc));
    }
    libusb_close(handle);
}

/*
 * The legacy backend reads status byte 0xf910 to select one of two CCD
 * parameter sets. This uses the same verified vendor-IN framing as the
 * revision and register-snapshot reads; it never sends a write command.
 */
static void read_ccd_profile(libusb_device *device) {
    libusb_device_handle *handle = NULL;
    unsigned char status = 0;
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  CCD profile: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  CCD profile: interface claim failed (%s)\n", libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        0x00, 0xf910, 0x0100, &status, 1, 1000);
    if (rc == 1) {
        unsigned int profile = (status & 0x02U) != 0U ? 1U : 0U;
        printf("  CCD profile status: 0x%02x\n", status);
        printf("  CCD profile: %u (%s USB-2 parameter set)\n", profile,
               profile == 0U ? "s_RtIniFile" : "t_RtIniFile");
    } else {
        printf("  CCD profile: failed (%s)\n",
               rc < 0 ? libusb_error_name(rc) : "unexpected response length");
    }
    int release_rc = libusb_release_interface(handle, 0);
    if (release_rc != LIBUSB_SUCCESS) {
        printf("  CCD profile release: %s\n", libusb_error_name(release_rc));
    }
    libusb_close(handle);
}

/*
 * CdRegRead() in the legacy 3770 backend performs this same vendor-IN read
 * sequence for the controller register bank. The resulting snapshot gives us
 * the scanner's idle state without changing that state. The destination is
 * created exclusively so the tool cannot accidentally overwrite a file.
 */
static void snapshot_registers(libusb_device *device, const char *output_path) {
    libusb_device_handle *handle = NULL;
    unsigned char registers[RTS8822_REGISTER_BYTES] = {0};
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  register snapshot: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  register snapshot: interface claim failed (%s)\n",
               libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        0x00, RTS8822_REGISTER_BASE, 0x0100, registers,
        RTS8822_REGISTER_BYTES, 2000);
    if (rc == RTS8822_REGISTER_BYTES) {
        int fd = open(output_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            printf("  register snapshot: cannot create %s (%s)\n", output_path,
                   strerror(errno));
        } else {
            ssize_t written = write(fd, registers, sizeof(registers));
            if (written == (ssize_t)sizeof(registers)) {
                printf("  register snapshot: wrote %u bytes to %s\n",
                       RTS8822_REGISTER_BYTES, output_path);
            } else {
                printf("  register snapshot: file write failed (%s)\n",
                       written < 0 ? strerror(errno) : "short write");
            }
            close(fd);
        }
    } else {
        printf("  register snapshot: failed (%s)\n",
               rc < 0 ? libusb_error_name(rc) : "unexpected response length");
    }
    int release_rc = libusb_release_interface(handle, 0);
    if (release_rc != LIBUSB_SUCCESS) {
        printf("  register snapshot release: %s\n", libusb_error_name(release_rc));
    }
    libusb_close(handle);
}

/*
 * Read-only backup of the controller NVRAM (0x00-0x7f). The hp3770 backend
 * stores calibration counters, the reference position (0x6a-0x6e) and lamp
 * PWM (0x7b) there, exactly like the original driver. Each byte is read the
 * way hp3900's RTS_EEPROM_ReadByte does it: a 2-byte vendor IN transfer with
 * wIndex 0x0200 (bRequest 0x00, as in the original 3770 driver).
 */
static void dump_nvram(libusb_device *device, const char *output_path) {
    libusb_device_handle *handle = NULL;
    unsigned char nvram[0x80] = {0};
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        printf("  NVRAM dump: unavailable (%s)\n", libusb_error_name(rc));
        return;
    }
    rc = libusb_claim_interface(handle, 0);
    if (rc != LIBUSB_SUCCESS) {
        printf("  NVRAM dump: interface claim failed (%s)\n", libusb_error_name(rc));
        libusb_close(handle);
        return;
    }
    for (unsigned int address = 0; address < sizeof(nvram); ++address) {
        unsigned char pair[2] = {0};
        rc = libusb_control_transfer(
            handle,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
            0x00, (uint16_t)address, 0x0200, pair, 2, 1000);
        if (rc != 2) {
            printf("  NVRAM dump: read of 0x%02x failed (%s)\n", address,
                   rc < 0 ? libusb_error_name(rc) : "unexpected response length");
            break;
        }
        nvram[address] = pair[0];
    }
    if (rc == 2) {
        int fd = open(output_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            printf("  NVRAM dump: cannot create %s (%s)\n", output_path, strerror(errno));
        } else {
            ssize_t written = write(fd, nvram, sizeof(nvram));
            close(fd);
            printf("  NVRAM dump: %s %u bytes to %s\n",
                   written == (ssize_t)sizeof(nvram) ? "wrote" : "FAILED writing",
                   (unsigned)sizeof(nvram), output_path);
            printf("  reference position 0x6a-0x6f:");
            for (unsigned int i = 0x6a; i < 0x70; ++i) printf(" %02x", nvram[i]);
            printf("\n  counters/PWM 0x76-0x7b:");
            for (unsigned int i = 0x76; i < 0x7c; ++i) printf(" %02x", nvram[i]);
            putchar('\n');
        }
    }
    int release_rc = libusb_release_interface(handle, 0);
    if (release_rc != LIBUSB_SUCCESS) {
        printf("  NVRAM dump release: %s\n", libusb_error_name(release_rc));
    }
    libusb_close(handle);
}

/* Analyze a captured register bank without opening or accessing a USB device. */
static int inspect_snapshot(const char *input_path) {
    unsigned char registers[RTS8822_REGISTER_BYTES] = {0};
    FILE *input = fopen(input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Cannot open snapshot %s: %s\n", input_path, strerror(errno));
        return 1;
    }
    size_t count = fread(registers, 1, sizeof(registers), input);
    int extra = fgetc(input);
    if (fclose(input) != 0) {
        fprintf(stderr, "Cannot close snapshot %s: %s\n", input_path, strerror(errno));
        return 1;
    }
    if (count != sizeof(registers) || extra != EOF) {
        fprintf(stderr, "Invalid snapshot %s: expected exactly %u bytes, got %zu%s\n",
                input_path, RTS8822_REGISTER_BYTES, count,
                extra == EOF ? "" : "+");
        return 1;
    }

    unsigned char video_status = registers[0xe812 - RTS8822_REGISTER_BASE];
    const unsigned char *pending = &registers[0xef16 - RTS8822_REGISTER_BASE];
    unsigned long pending_bytes = (unsigned long)pending[0] |
                                  ((unsigned long)pending[1] << 8) |
                                  ((unsigned long)pending[2] << 16);
    printf("RTS8822 register snapshot: %s (%u bytes)\n", input_path,
           RTS8822_REGISTER_BYTES);
    printf("  register 0xe800: 0x%02x\n", registers[0]);
    printf("  register 0xe801: 0x%02x\n", registers[1]);
    printf("  video status 0xe812: 0x%02x\n", video_status);
    printf("  pending image bytes 0xef16: %lu\n", pending_bytes);
    printf("  register 0xe946: 0x%02x\n", registers[0x146]);
    printf("  register 0xee00: 0x%02x\n", registers[0x600]);
    return 0;
}

/*
 * Build the independently verified portion of a 100 dpi profile-1 register
 * image in memory and report the delta. This intentionally has no USB path
 * and does not write a candidate image to disk: motor, calibration, and scan
 * geometry fields still require further reconstruction.
 */
static int plan_gray100(const char *input_path) {
    /*
     * Retired: this partial planner encoded the t_Timing8 (600 dpi) table as
     * if it were Timing 10. The complete register setup now lives in the
     * hp3770 backend (backend/, used by hp3770-scan), which reuses the
     * hardware-proven RTS8822 engine of SANE's hp3900 backend.
     */
    (void)input_path;
    fputs("--plan-gray100 has been retired: it used the 600 dpi timing table.\n"
          "Register setup is now done by the hp3770 backend; see hp3770-scan.\n",
          stderr);
    return 64;
}

#if 0 /* original partial planner, kept for reference (uses the wrong timing table) */
static int plan_gray100_retired(const char *input_path) {
    unsigned char registers[RTS8822_REGISTER_BYTES] = {0};
    unsigned char candidate[RTS8822_REGISTER_BYTES] = {0};
    const struct hp3770_scan_profile *profile = hp3770_usb2_gray_profile(100);
    FILE *input = fopen(input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Cannot open snapshot %s: %s\n", input_path, strerror(errno));
        return 1;
    }
    size_t count = fread(registers, 1, sizeof(registers), input);
    int extra = fgetc(input);
    if (fclose(input) != 0) {
        fprintf(stderr, "Cannot close snapshot %s: %s\n", input_path, strerror(errno));
        return 1;
    }
    if (count != sizeof(registers) || extra != EOF) {
        fprintf(stderr, "Invalid snapshot %s: expected exactly %u bytes, got %zu%s\n",
                input_path, RTS8822_REGISTER_BYTES, count,
                extra == EOF ? "" : "+");
        return 1;
    }

    if (profile == NULL) {
        fputs("Internal error: 100 dpi profile is unavailable\n", stderr);
        return 1;
    }

    memcpy(candidate, registers, sizeof(candidate));
    /* RTS8822: register 0x00 low nibble is the system-clock selector. */
    candidate[0x00] = (unsigned char)((candidate[0x00] & 0xf0U) |
                                      profile->system_clock);
    /* RTS8822: register 0xc0 low five bits are sensor-DPI / requested-DPI. */
    candidate[0xc0] = (unsigned char)((candidate[0xc0] & 0xe0U) |
                                      (profile->sensor_resolution / profile->dpi));
    /* RTS8822 motor setup uses bits 6..4 for the selected step type. */
    candidate[0xd9] = (unsigned char)((candidate[0xd9] & 0x8fU) |
                                      (profile->motor_step_type << 4));
    /* The high nibble is the profile's dummy-line multiplier. */
    candidate[0xd6] = (unsigned char)((candidate[0xd6] & 0x0fU) |
                                      (profile->dummy_lines << 4));
    /* A non-negative motor-curve selection enables the curve flag. */
    candidate[0xdf] = (unsigned char)(candidate[0xdf] | 0x10U);
    /* CTPC is the 24-bit line exposure counter, stored little-endian. */
    candidate[0x30] = (unsigned char)(profile->ctpc & 0xffU);
    candidate[0x31] = (unsigned char)((profile->ctpc >> 8) & 0xffU);
    candidate[0x32] = 0;

    /* Profile-1 Timing 10 table, encoded using the RTS8822 timing register map. */
    candidate[0x45] = 0x04; /* CVTRP1..3=0, CVTRFPW=4 */
    candidate[0x46] = (unsigned char)((candidate[0x46] & 0xe0U) | 0x04U);
    candidate[0x47] = 0x0f; /* CVTRW */

    static const uint64_t cph_p1[6] = {
        UINT64_C(0x000000000), UINT64_C(0xE00007000), UINT64_C(0xFFFFFF000),
        UINT64_C(0xFFFFFF000), UINT64_C(0x000FFF000), UINT64_C(0xFFF000000),
    };
    static const uint8_t cph_go[6] = {0, 0, 1, 1, 0, 1};
    static const unsigned int cph_offsets[6] = {0x48, 0x52, 0x5c, 0x66, 0x70, 0x7a};
    for (unsigned int phase = 0; phase < 6; ++phase) {
        unsigned int offset = cph_offsets[phase];
        uint64_t value = cph_p1[phase];
        candidate[offset + 0] = (unsigned char)value;
        candidate[offset + 1] = (unsigned char)(value >> 8);
        candidate[offset + 2] = (unsigned char)(value >> 16);
        candidate[offset + 3] = (unsigned char)(value >> 24);
        candidate[offset + 4] = (unsigned char)((candidate[offset + 4] & 0x80U) |
                                                 ((value >> 32) & 0x0fU) |
                                                 0x20U | (cph_go[phase] << 4));
        candidate[offset + 5] = 0;
        candidate[offset + 6] = 0;
        candidate[offset + 7] = 0;
        candidate[offset + 8] = 0;
        candidate[offset + 9] = (unsigned char)(candidate[offset + 9] & 0xf0U);
    }

    /* CPH back-porch and clamp windows: 0, 0, 1, and 0x550 respectively. */
    memset(candidate + 0x84, 0, 6);
    candidate[0x8a] = 0x01;
    candidate[0x8b] = 0x00;
    candidate[0x8c] = 0x00;
    candidate[0x8d] = 0x50;
    candidate[0x8e] = 0x05;
    candidate[0x8f] = 0x00;

    candidate[0x92] = (unsigned char)((candidate[0x92] & 0xc0U) | 0x09U);
    candidate[0x93] = (unsigned char)((candidate[0x93] & 0xc0U) | 0x0cU);
    candidate[0x94] = (unsigned char)((candidate[0x94] & 0xc0U) | 0x12U);
    candidate[0x95] = (unsigned char)((candidate[0x95] & 0xc0U) | 0x14U);
    candidate[0x96] = (unsigned char)((candidate[0x96] & 0xc0U) | 0x17U);

    /* ADC clock 1 = 0x00fff0000; clock 2 and its extension bit are zero. */
    candidate[0x97] = 0x00;
    candidate[0x98] = 0x00;
    candidate[0x99] = 0xff;
    candidate[0x9a] = 0x0f;
    candidate[0x9b] = (unsigned char)(candidate[0x9b] & 0xf0U);
    candidate[0xc1] = 0x00;
    candidate[0xc2] = 0x00;
    candidate[0xc3] = 0x00;
    candidate[0xc4] = 0x00;
    candidate[0xc5] = (unsigned char)(candidate[0xc5] & 0xe0U);

    puts("HP ScanJet 3770 offline partial register plan");
    puts("  mode: CCD profile 1, reflective grayscale, 100 dpi");
    printf("  sensor timing resolution: %u dpi (ratio %u:1)\n",
           profile->sensor_resolution, profile->sensor_resolution / profile->dpi);
    puts("  source: supplied idle register snapshot");
    for (unsigned int i = 0; i < sizeof(candidate); ++i) {
        if (candidate[i] != registers[i]) {
            printf("  register 0x%04x: 0x%02x -> 0x%02x\n",
                   RTS8822_REGISTER_BASE + i, registers[i], candidate[i]);
        }
    }
    puts("  not yet compiled: per-channel exposure caps, motor-curve tables/backstep,");
    puts("                    scan geometry,");
    puts("                    gain/offset, gamma, shading, DMA");
    puts("  status: offline analysis only; no scanner command sent");
    return 0;
}

#endif

/* Display the actual RTS8822 sensor-timing bank preserved in a snapshot. */
static int inspect_timing(const char *input_path) {
    unsigned char registers[RTS8822_REGISTER_BYTES] = {0};
    FILE *input = fopen(input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Cannot open snapshot %s: %s\n", input_path, strerror(errno));
        return 1;
    }
    size_t count = fread(registers, 1, sizeof(registers), input);
    int extra = fgetc(input);
    if (fclose(input) != 0) {
        fprintf(stderr, "Cannot close snapshot %s: %s\n", input_path, strerror(errno));
        return 1;
    }
    if (count != sizeof(registers) || extra != EOF) {
        fprintf(stderr, "Invalid snapshot %s: expected exactly %u bytes, got %zu%s\n",
                input_path, RTS8822_REGISTER_BYTES, count,
                extra == EOF ? "" : "+");
        return 1;
    }

    static const struct {
        unsigned int offset;
        const char *label;
        unsigned int length;
    } fields[] = {
        {0x45, "transfer-gate controls", 3},
        {0x48, "CPH phase 0", 10},
        {0x52, "CPH phase 1", 10},
        {0x5c, "CPH phase 2", 10},
        {0x66, "CPH phase 3", 10},
        {0x70, "CPH phase 4", 10},
        {0x7a, "CPH phase 5", 10},
        {0x84, "CPH/clamp start", 15},
        {0x92, "CDS and CNPP", 5},
        {0x97, "ADC clock 1", 5},
        {0xc1, "ADC clock 2", 5},
    };

    puts("RTS8822 sensor-timing snapshot");
    for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        printf("  0x%04x %-24s", RTS8822_REGISTER_BASE + fields[i].offset,
               fields[i].label);
        for (unsigned int j = 0; j < fields[i].length; ++j) {
            printf(" %02x", registers[fields[i].offset + j]);
        }
        putchar('\n');
    }
    puts("  status: snapshot decoding only; no scanner command sent");
    return 0;
}

/* Print a recovered scan profile without accessing USB hardware. */
static int show_usb2_gray_profile(const char *dpi_text) {
    char *end = NULL;
    unsigned long dpi = strtoul(dpi_text, &end, 10);
    if (dpi_text[0] == '\0' || *end != '\0' || dpi > 2400) {
        fprintf(stderr, "Invalid DPI: %s\n", dpi_text);
        return 64;
    }
    const struct hp3770_scan_profile *profile =
        hp3770_usb2_gray_profile((unsigned int)dpi);
    if (profile == NULL) {
        fprintf(stderr, "Unsupported USB-2 reflective grayscale DPI: %lu\n", dpi);
        return 64;
    }
    printf("HP ScanJet 3770 CCD profile 1 USB-2 reflective grayscale plan (%u dpi)\n",
           profile->dpi);
    printf("  sensor timing resolution: %u dpi\n", profile->sensor_resolution);
    printf("  sample rate: %u\n", profile->sample_rate);
    printf("  system clock: %u\n", profile->system_clock);
    printf("  motor step type: %u\n", profile->motor_step_type);
    printf("  CTPC: %u\n", profile->ctpc);
    printf("  timing: %u\n", profile->timing);
    printf("  motor curve: %u\n", profile->motor_curve);
    printf("  motor backstep: %u\n", profile->motor_backstep);
    printf("  dummy lines: %u\n", profile->dummy_lines);
    puts("  status: parameter plan only; no scanner command sent");
    return 0;
}

int main(int argc, char **argv) {
    int claim_only = argc == 2 && strcmp(argv[1], "--claim") == 0;
    int read_capability = argc == 2 && strcmp(argv[1], "--capability") == 0;
    int read_video_length = argc == 2 && strcmp(argv[1], "--video-length") == 0;
    int read_revision = argc == 2 && strcmp(argv[1], "--revision") == 0;
    int read_ccd = argc == 2 && strcmp(argv[1], "--ccd-profile") == 0;
    int plan = argc == 3 && strcmp(argv[1], "--plan-gray100") == 0;
    int timing = argc == 3 && strcmp(argv[1], "--inspect-timing") == 0;
    int snapshot = argc == 3 && strcmp(argv[1], "--snapshot-registers") == 0;
    int nvram = argc == 3 && strcmp(argv[1], "--dump-nvram") == 0;
    int inspect = argc == 3 && strcmp(argv[1], "--inspect-snapshot") == 0;
    int profile = argc == 3 && strcmp(argv[1], "--show-usb2-gray-profile") == 0;
    if (argc > 1 && !claim_only && !read_capability && !read_video_length &&
        !read_revision && !read_ccd && !snapshot && !nvram && !inspect && !profile && !plan && !timing) {
        fprintf(stderr, "Usage: %s [--claim | --capability | --video-length | --revision | --ccd-profile | --snapshot-registers FILE | --dump-nvram FILE | --inspect-snapshot FILE | --inspect-timing FILE | --plan-gray100 FILE | --show-usb2-gray-profile DPI]\n",
                argv[0]);
        return 64;
    }
    if (inspect) return inspect_snapshot(argv[2]);
    if (timing) return inspect_timing(argv[2]);
    if (plan) return plan_gray100(argv[2]);
    if (profile) return show_usb2_gray_profile(argv[2]);

    puts("hp3770-probe 0.14 (read-only USB diagnostics)");
    libusb_context *ctx = NULL;
    libusb_device **devices = NULL;
    int rc = libusb_init(&ctx);
    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }

    ssize_t count = libusb_get_device_list(ctx, &devices);
    if (count < 0) {
        fprintf(stderr, "libusb_get_device_list failed: %s\n", libusb_error_name((int)count));
        libusb_exit(ctx);
        return 1;
    }

    int found = 0;
    for (ssize_t index = 0; index < count; ++index) {
        struct libusb_device_descriptor device_desc;
        libusb_device *device = devices[index];
        rc = libusb_get_device_descriptor(device, &device_desc);
        if (rc != LIBUSB_SUCCESS || device_desc.idVendor != HP_VENDOR_ID ||
            device_desc.idProduct != SJ3770_PRODUCT_ID) continue;

        found = 1;
        printf("HP ScanJet 3770 found: bus=%u address=%u usb=%x.%02x device_class=0x%02x configurations=%u\n",
               libusb_get_bus_number(device), libusb_get_device_address(device),
               device_desc.bcdUSB >> 8, device_desc.bcdUSB & 0xff,
               device_desc.bDeviceClass, device_desc.bNumConfigurations);
        print_identity_strings(device, &device_desc);
        if (claim_only) test_interface_claim(device);
        if (read_capability) read_usb2_capability(device);
        if (read_video_length) read_pending_video_length(device);
        if (read_revision) read_chip_revision(device);
        if (read_ccd) read_ccd_profile(device);
        if (snapshot) snapshot_registers(device, argv[2]);
        if (nvram) dump_nvram(device, argv[2]);

        for (uint8_t config_index = 0; config_index < device_desc.bNumConfigurations; ++config_index) {
            struct libusb_config_descriptor *config = NULL;
            rc = libusb_get_config_descriptor(device, config_index, &config);
            if (rc != LIBUSB_SUCCESS) {
                printf("  configuration %u: unavailable (%s)\n", config_index, libusb_error_name(rc));
                continue;
            }
            print_configuration(config);
            libusb_free_config_descriptor(config);
        }
    }

    if (!found) {
        fprintf(stderr, "HP ScanJet 3770 (03f0:2505) not found. Connect it directly and retry.\n");
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(ctx);
    return found ? 0 : 2;
}
