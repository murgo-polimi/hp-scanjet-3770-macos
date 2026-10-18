# HP ScanJet 3770 on macOS: porting notes

This document explains how the driver is built, how it was checked against
HP's own driver, and exactly where it departs from known-good code. Read it
before changing the backend or reporting a hardware result.

## Key finding: the 3770 uses HP's "t6" profile, not the HP3970's

HP's 3770 Linux backend (`libsane-hp3770.so.1.0.13`, HP/Realtek
"CFitApi"/"CChipDriver" SDK) carries three complete scanner profiles as INI
text blobs in `.data`: `s_` (Sony CCD), `t_` (Toshiba, 2400 dpi) and `t6_`
(1200 dpi), each with `RtIniFile`, `USB1IniFile` and `Timing1..12`, plus
`MotorCurve1..12` and `FitCalibrate`.

`CFitApi::OpenDeviceDriver` reads `FitCalibrate` `[PLATFORM] CCDType`. A
value of 0 or more overrides the GPIO-based detection; the 3770 ships with
`CCDType=2`, and `Ke_InitProfileName` maps 0/1/2 to `s_`/`t_`/`t6_`. So
**every 3770 runs the `t6_` profile**, whatever its status byte says.

The first version of this driver assumed the 3770 was an HP3970 (the `t_`
profile, which matches hp3900's HP3970 tables almost exactly) and produced
dark, banded images on the real scanner. The `t6_` profile differs
everywhere that matters:

| | t6 (3770) | HP3970 / t_ |
| --- | --- | --- |
| Optical resolution | 1200 dpi CCD | 2400 dpi TCD2952 |
| Motor resolution | 1200 | 2400 |
| Scan modes | 1200/600/300/200/100, own CTPC, clocks, curves, back steps | 2400…100 |
| Timings | `t6_Timing1..12` | `t_Timing1..12` |
| Motor curves | 12 (`MotorCurve1..12`) | 8 |
| Reference voltages VRTS/VRMS/VRBS | 2/3/2 | 0/0/0 |
| Fixed lamp PWM duty | 0, PWM frequency 9 | 22 |
| SER/LER per resolution | own table | hp3900 table |
| Lineart thresholds | 100/99 | 128/127 |

`tools/gen_legacy_tables.py` reads the profile named by `CCDType` straight
from the ELF file and writes `backend/hp3770_legacy_tables.c`
(`make tables`). The legacy INI parser (`GetInitializeData`) uses only the
first number of a value such as `CTPC=2999 3983 3007`; the generator does the
same. Key and section names are matched case-insensitively, like `StrNiCmp`.

## Verification against HP's driver (emulator)

HP's driver is an i386 Linux library. It was run unmodified on an x86 Linux
host under a small harness (`dlopen` plus a simulated RTS8822 that logs every
USB transfer); this driver was run against the equivalent simulator in
`tests/sanei_usb_mock.c`. The register image the chip receives for the final
image scan was then compared register by register for colour 100, 200, 300
and 600 dpi.

After the changes below, every sensor-timing and motor register agrees
(CCD clocks, CTPC/line period, exposure, step size, acceleration tables,
x-sampling, gain-mode bits, AFE reference voltages, CRVS, static chip
configuration). The remaining differences are understood:

- **AFE offsets and VGA gains** (e814-e825): HP's driver calibrates them in
  hardware; its values in the emulator only reflect the synthetic data. This
  driver programs fixed values from the profile (below).
- **Geometry** (e8b0-e8b3, e8d0-e8d3): the scan area and reference position
  differ between the two runs, not the formula.
- **RAM placement** of the motor tables (e8f6-e903, e905…) and the shading /
  buffer layout (e9b7-e9d0, ef08-ef11): different addresses, same content.
- **Hardware shading and gamma** (e9cf/e9d0): HP's driver uses the chip's
  shading; hp3900 corrects white shading in software.
- Lamp-off timer minutes (e947) and the home-sensor status bit (e96f bit 6).

**1200 dpi** (compared later, colour): timing, motor and line-distance
registers agree as at 600 dpi (`MotorPhase=1` is set for 600 dpi too, so it
is not specific to 1200). Two things differed and now follow HP's driver:

- Even/odd pixel distance: at the CCD's full resolution HP's driver programs
  e949/e94b/e94d = 4, 28, 52 (SensorEvenOddDistance 4, plus the colour line
  distances), so the chip realigns the CCD's staggered even and odd pixels.
  hp3900 only did this above 1200 dpi (its 2400 dpi CCD); for HP3770 the
  image pass now sets `highresolution` at 1200 dpi. Lines stay arranged by
  the chip; the calibration passes keep 0, as in HP's trace.
- e954/e955 = 0x0040 at 1200 dpi for every pass (bits 7, 8 and 11 clear;
  0x0980 up to 600 dpi), in `Lamp_SetGainMode`.

The remaining differences are the ones listed above for 100-600 dpi.

## Architecture

```
frontend/hp3770_scan.c      CLI (SANE frontend, writes TIFF/PNM)
frontend/hp3770_airscan.c   eSCL/AirScan server for Mac apps (JPEG/PDF)
frontend/hp3770_common.c    shared: options by name, ripple removal
                            (hp3770_deripple.c), paper white
                            (hp3770_autowhite.c)
frontend/hp3770_jpeg.c      baseline JPEG encoder and one-page PDF writer
        |  sane_hp3770_* entry points (SANE API, statically linked)
backend/hp3770.c            backend wrapper (BACKEND_NAME hp3770)
backend/hp3770_sane.c       SANE options layer          } from hp3900 1.2.1
backend/hp3770_rts8822.c    RTS8822 engine              } with the changes
backend/hp3770_config.c     per-model configuration     } listed below
backend/hp3770_usb.c        vendor-request transport    }
backend/hp3770_legacy_tables.c   GENERATED from the 3770 driver (t6 profile)
compat/sanei_usb_libusb.c   sanei_usb on libusb-1.0 (macOS user space)
compat/sanei_misc.c, ...    sanei config/debug/constraint helpers
tests/sanei_usb_mock.c      simulated RTS8822 for tests without hardware
```

No kernel extension or system modification is needed: the scanner exposes a
vendor-specific interface that macOS leaves unclaimed, and libusb claims it
from user space.

## Changes to the hp3900 engine

All changes are marked in the source with `HP3770`; the complete diff
against sane-backends 1.2.1 is in
[`hp3770-vs-hp3900-1.2.1.diff`](hp3770-vs-hp3900-1.2.1.diff).

1. New model `HP3770` (USB 03f0:2505, chipset RTS8822L-01H). Device, chipset,
   buttons, SSCG, constraints and auto-reference rows of its own; sensor
   (1200 dpi, line distance 24, even/odd 4) and motor (1200, base speed 800,
   moves) from the profile.
2. Generated t6 tables for timings, scan modes, motor moves, motor curves,
   SER/LER, reference voltages, fixed PWM, lineart thresholds, calibration
   starting values, shading cut, lamp stability check and default AFE values.
3. CCD: always the t6 profile (`CCD_SENSOR`). Status byte 0xf910 is still read
   (read-only) and shown by `--info`, but, as in HP's driver, it does not
   select anything. The earlier refusal of a "Sony" unit is gone.
4. Register programming aligned with HP's driver traces:
   - 0xe946 low nibble = lamp PWM frequency (9);
   - CRVS (0xe810 bits 0-4) kept at the profile value 12 (hp3900 let the
     chip's idle value 14 through);
   - 0xe951 = 0x03, 0xe963/0xe965 bits 6-7 = 3, 0xe964 bit 7 = 0;
   - gain-mode word 0xe954: bits 7-8 set, bits 4-5 = 3 (≤100 dpi), 1 (≤300),
     0 (600); 0x0040 at 1200 dpi;
   - even/odd pixel distance (0xe949/0xe94b/0xe94d) programmed at 1200 dpi;
   - CLAMPE (0xe88d) = line period when the timing table holds all ones;
   - motor back step after buffer-full (0xe8dc) uses +2 like HP's driver.
5. AFE without hardware calibration: hp3900 writes a fixed HP3970 register
   block that also clears the reference-voltage bits. The HP3770 programs
   the profile's own values instead (offsets `DCOffsetEven/Odd1/2` of
   `[SCAN_CALI]`, PAG 3, VGA 4/4) for both the white-shading pass and the
   scan, so the shading always matches the scan gain. `HP3770_AFE=hp3970`
   restores the old block for comparison.
6. Gray and lineart are scanned in colour and converted in software, like
   HP's driver ("true gray"; it never uses the chip's gray mode).
   `--set opt_emulategray=no` uses the chip's gray mode instead.
7. USB transport: `bRequest` 0x00 and 64-byte write splitting, as in
   `CChipDriver::USBVendorRequest`/`USBExecCommand`.
8. Flatbed is the only scan source (no transparency adapter).
9. Safety nets for the motor start position: a reference position read from
   NVRAM (checksum-valid but outside x 1..600, y 1..1200 at 600 dpi) is
   discarded and re-detected; an implausible detected position aborts the
   scan instead of being used or saved. `HP3770_AUTOREF_X/Y` overrides are
   only accepted within 0..400 and 300..1000.
10. Bug fixes found with AddressSanitizer in the simulator: `Motor_Setup_Steps`
    over-read when padding the motor-step transfer; `IRead_Integer` signed
    shift; NULL device list and a claimed interface left behind in
    `sane_open`; `hp3970_gainoffset()` never matching (loop bound), which
    left PAG 0 for the scan while shading ran at PAG 3 (first, dark image).
11. Memory leaks that only matter in the long-running `hp3770-airscan`
    (about 54 KB per scan, found with LeakSanitizer): `sane_close` never
    freed the scanner handle, `Load_MotorCurves` the curve buffer,
    `Motor_Curve_Free` the curve pointer arrays and `WShading_Calibrate` its
    four work buffers. About 200 bytes per scan of upstream option strings
    remain.

hp3900's hardware calibration path (`calibrate = TRUE`, what HP's driver
does) stays off: under AddressSanitizer it overruns a stack buffer in
`fn3330`/`Shading_black_apply`.

## Mac apps: hp3770-airscan

macOS dropped third-party TWAIN/ICA scanner modules from its apps, but Image
Capture, Preview and ImageCaptureCore talk to eSCL ("AirScan") network
scanners without drivers. `hp3770-airscan` is such a scanner:

- HTTP/1.1 server (thread per connection, keep-alive): `ScannerCapabilities`
  (platen 2551 x 3543 in 1/300 inch, RGB24 and Grayscale8, 75-1200 dpi,
  image/jpeg and application/pdf), `ScannerStatus` with `JobInfo`,
  `POST ScanJobs` (201 + Location), `NextDocument` (one page, then 404) and
  `DELETE` (cancel at the next read).
- Each job runs in a worker thread: `sane_init`, `sane_open`, options, scan
  8-bit into memory, `sane_close`, `sane_exit`, then ripple removal, paper
  white and JPEG (quality 90). The USB device is held only during a scan.
- A scan takes up to ~90 s before the first byte exists, longer than client
  idle timeouts. `NextDocument` therefore starts a chunked response at once
  with the start of the file (JPEG SOI + JFIF APP0, or the PDF header) and
  then sends a JPEG COM segment or a PDF comment line every 2 s. The final
  PDF's xref offsets include the bytes already sent
  (`pdf_wrap_jpeg(..., sent)`).
- Bonjour `_uscan._tcp` (TXT: rs=eSCL, vers=2.63, pdl, cs, is=platen, UUID,
  ty) through dns_sd. Default: registered on the local-only interface and
  the server answers only peers that are loopback or the address the
  connection was made to, i.e. this Mac. `--announce-network` registers on
  all interfaces (still this Mac only); `--share` lifts both limits.

Tested against the simulator with curl (streaming and already-finished
fetches, busy 503, cancel, unknown job, PDF validity with qpdf, under
ASan/UBSan) and with sane-airscan's `scanimage` as an independent eSCL
client. Works with Image Capture and Preview on macOS Tahoe.

## Run-time switches

| Variable / option | Effect |
| --- | --- |
| `SANE_DEBUG_HP3770=1..4` | backend debug output (4 = every USB transfer) |
| `HP3770_USB_TRACE=file` | log of every control/bulk transfer (writes with data) |
| `HP3770_USB_REQUEST=0x04` | use hp3900's bRequest instead of 0x00 |
| `HP3770_USB_WRITE_CHUNK=0` | send register writes unsplit (hp3900 behaviour) |
| `HP3770_AFE=hp3970` | hp3900's fixed HP3970 AFE block instead of the t6 values |
| `HP3770_HP3970_CALIBRATION=1` | HP3970 calibration starting values |
| `HP3770_AUTOREF_X`, `HP3770_AUTOREF_Y` | origin fine-tuning (2400 dpi units; defaults 88, 650; accepted 0..400 and 300..1000) |
| `HP3770_LAMP_PWM=0..63` or `off` | force the lamp PWM duty cycle, or disable PWM dimming |
| `--set opt_emulategray=no` | gray from the chip's gray mode instead of colour |
| `--set opt_model=HP3970` | run with hp3900's HP3970 profile (wrong for the 3770; comparison only) |

## Hardware history

**2026-10-18, first scans (HP3970 profile).** Open, park, RAM detection,
lamp warm-up, reference detection and a 100 dpi gray scan all ran on the real
scanner, so the transport and motor basics work. The image was too dark (PAG
mismatch) and showed horizontal banding with an exact 8-line period. Both are
consistent with the wrong CCD profile (2400 dpi timings and AFE on a 1200 dpi
sensor), which led to the t6 analysis above.

**2026-10-18, second run (t6 profile): head stalled and whistled.** The
trace showed two causes, both in hp3900 code paths that HP's driver does not
take, and neither visible in the emulator (whose head is always "home"):

1. hp3900 copies the 100 dpi line period into the 600 dpi reference-detection
   pass when the user scans at 100 dpi. With the t6 timings that left four
   motor steps in a 3000-clock line: a constant ~2700 steps/s without
   acceleration. HP's driver keeps the pass's own period (11999). The copy is
   now skipped for the HP3770.
2. Head moves (park, relocate) used the t6 `[MotorMove1]` values (system clock
   4, MotorCurve9). hp3900's `Motor_Move` fixes the pixel-clock divider at 12,
   so this doubled the step rate of the HP3970 move that had parked this
   scanner correctly; every park ran into the 15 s timeout. The HP3770 now uses
   that proven move (clock 2, CTPC 6431, half steps, MotorCurve2, identical in
   the 3770 driver); `HP3770_T6_MOTORMOVE=1` restores the t6 values.

Also: a park that is still running after 15 s now stops the motor, and the
stop in `RTS_Scanner_StopScan` writes the execution bit to 0xe800 (upstream
writes it to address 0x0000, so a running job was never stopped). The
simulator now models the home sensor, so parks are exercised in tests.

**2026-10-18, third run: first real image.** A 300 dpi colour scan of a
printed page came out sharp and correctly coloured, with two remaining
problems:

1. **Half brightness** (white paper ~105/255). hp3900's software shading only
   flattens the white reference to its own brightest pixel, so white keeps the
   raw sensor level (the white strip read ~28 000-30 700 of 65 535 with the
   profile's AFE values). The HP3770 now scales the reference to a fixed white
   level (`HP3770_WHITE_LEVEL`, default 92 % of full scale, gain capped at 8).
2. **A park that never ended** after the white-shading pass, which moves the
   head only ~3 mm (296 half-steps) from home; parks from 10 mm and more were
   fine, as was the HP3970-profile park after a 7 mm shading pass. The driver
   now tracks the head distance from the jobs it starts and, before a park
   from closer than 700 half-steps, first moves forward slowly (400 steps/s,
   no ramp needed). `Motor_Move` waits at most 15 s and then stops the motor
   (upstream waited 10 + 20 s), and its curve-less moves use their own system
   clock for the step rate.

The fine horizontal colour stripes in a pink printed area of that page are
moiré between the print's halftone screen and the 300 dpi sampling (the
green channel varies most, as magenta ink absorbs green); white areas vary by
only ~3 %.

**2026-10-18, diagnostic series (t6 profile, all fixes): every scan works.**
Colour 100/300/600 dpi, gray via colour and via the chip, raw and the old AFE
block all completed in 31-44 s with no stalls; the forward nudge before short
parks was used and every park finished. Findings:

- White paper now ~215-226 (R, G) and ~245 (B); black ink ~12. The old HP3970
  AFE block lifts blacks to ~44, so the t6 offsets stay. Gray via colour is
  cleaner than the chip's gray mode, which stays off by default.
- Vertical geometry agrees at all resolutions. Horizontally, 300 and 600 dpi
  agree, but 100 dpi started 4 mm further right: hp3900 adds a 24-pixel
  lead-in in the scan resolution. For the HP3770 it is now 24 pixels at 300 dpi
  for lower resolutions (8 at 100 dpi).
- **Lamp ripple at 100 Hz.** White areas show line-to-line brightness bands
  (about ±4 % raw, ±3 % after shading per channel) that repeat every 10 ms:
  10/3 lines at 300 dpi (3 ms lines), aliased to 2.5 lines at 600 dpi (6 ms
  lines). R, G and B are out of phase because the CCD's colour rows see each
  paper line at different times, so the bands are coloured. Present in the raw
  data, so not a shading artefact. Cause still open: lamp PWM (frequency
  nibble 9 now, 0 before) or mains ripple / room light. `tools/lamp-test.sh`
  scans plain paper with PWM off, duty 63 and frequency 0/15
  (`HP3770_PWM_FREQ=0..15`).
- White has a slight blue cast (blue reaches 255 on paper while R/G are ~220):
  the paper reflects more blue than the calibration strip (optical
  brighteners). HP's driver also maps the strip to the same level in every
  channel (`WRef` 245, cut -6).

**Lamp ripple test (plain paper, room lit and dark).** The bands are the
same in all ten scans: PWM default, off, duty 63, frequency nibble 0 and 15,
with the room lights on and off (100 Hz component 6-11 grey levels in every
case). So it is neither the chip's lamp PWM nor room light: the lamp's own
supply carries twice-mains ripple. HP's driver uses the same 3 ms line period
at 300 dpi, so it is a property of the scanner, not of this driver.

Fix in `hp3770-scan` (`frontend/hp3770_deripple.c`): after the scan, each
line's brightness relative to its neighbours is measured per channel (median
over the line, so content cancels), the ripple frequency (from the line
period the backend reports, 100 or 120 Hz folded into the line rate) and its
second harmonic are fitted in overlapping 200-line windows, and divided out
(at most 10 %). On the real scans the 100 Hz component drops from 6-11 to
0.1-0.5 grey levels at 300 dpi and from 2-3 to ~0.35 at 600 dpi; images
without a clear ripple are left untouched. ~0.5 s for an A4 300 dpi page.
`--no-deripple` switches it off, `HP3770_MAINS=50|60` fixes the mains
frequency.

Confirmed on the scanner: a 300 dpi colour scan of a colour chart has no
visible bands left (100 Hz component ~0.1 grey levels).

**A4 scans 1.33x too bright (upstream read bug).** An A4 scan came out with
every patch 1.33x brighter than a 100 mm scan of the same page, all channels
alike, reproducibly. The logs showed the white reference of the A4 run at
75 % of the small one. Cause: `RTS_GetImage_GetBuffer` counts the remaining
size in 16-bit words but subtracted bytes, so any image read above
262 080 bytes stopped after its first chunk. The A4 shading pass (357 120
bytes) got 73 % of its 24 lines; the rest of the buffer stayed zero, the
averages came out 0.73x and the gains 1.36x. Fixed (word/byte accounting),
which also makes the reported transfer size right. The simulator now reports
its buffer fill level during a job like the real chip, so multi-chunk
calibration reads are exercised.

The pale rectangle below the chart, in the same place in every A4 scan, is
the film window in the 3770's lid showing through the paper.

Confirmed on the scanner: the A4 shading pass now reads in two chunks, its
white reference matches the small scan (max ~32 600/34 400/33 100) and the
colour patches of an A4 scan agree with a 100 mm scan of the same page within
0.5 %. White paper comes out ~205/201/234 (no clipping): slightly grey and
blue, since the paper is darker and bluer than the calibration strip.

**A4 at 600 dpi (hardware).** 4960 x 7015 px, complete, 78 s including
warm-up and calibration, no read timeouts or stalls. Downsampled to 300 dpi it
agrees with the 300 dpi A4 scan within 2-3 px (0.2 mm) in position and
within 1-2 % in brightness; text and patch edges are sharp with no colour
fringing. The lamp ripple is much weaker at 600 dpi (6 ms lines integrate
60 % of a 10 ms cycle): 0.15/0.5/0.8 % here, below the correction's
threshold and hardly visible.

The ripple correction now also checks the phase step between the R, G and B
rows (2*pi*f*line distance, line distance = dpi/50 output lines): the real
ripple matches it (e.g. -66/-62 degrees vs -72 expected at 300 dpi), periodic
image content does not (0/0 degrees on the simulator's test pattern, which
the old check would have "corrected" by up to 10 %). The image scan's line
period is now taken from the last forward non-move job, so 600 dpi (whose
image job has no motor curve) is recognised too.

**Automatic paper white point** (`frontend/hp3770_autowhite.c`, on by
default, `--no-auto-white` to disable). The backend maps the calibration strip
to the same level in every channel; this paper is darker and bluer than the
strip, so its white came out ~205/201/234. After the scan (and the ripple
removal) the per-channel median of the brightest 20 % of the pixels is taken
as the paper white and each channel is scaled so it becomes
`HP3770_WHITE_TARGET` (245). Only used when those pixels form a tight cluster
((p98 - p80) / p90 of the luminance <= 0.10; documents 0.04-0.07, a photo
crop 0.35), the white is bright (>= 150) and only mildly tinted (max/min
<= 1.35); gains limited to 0.85-1.5. On the A4 chart scan: paper
216/213/244 -> gains 1.13/1.15/1.00, background ~232/231/235 (neutral).

The ripple correction is now skipped for single-channel images: gray is
computed from colour, where the R/G/B phases largely cancel (~0.5 % left),
and without three channels the phase check against image content is not
possible (the simulator's test pattern was otherwise "corrected").

Confirmed on the scanner (A4, 300 dpi): blank paper 232.6/231.2/235.0,
top margin 241/241/243, under 0.5 % clipped; patch colours scaled by
1.13/1.14/0.99 exactly as computed offline.

**2026-10-19, Mac apps.** `hp3770-airscan` worked with Image Capture on
the first try (overview scan, then colour scans), with the scanner announced
on the local-only Bonjour interface.

**2026-10-19, 1200 dpi.** First 1200 dpi colour scan (50 x 50 mm of the colour chart) after the
even/odd change: even and odd pixel columns line up within 0.2 px on
horizontal lines (an uncorrected stagger would be 4 lines), colour
registration on thin lines is within 0.2-0.4 px as at 600 dpi, the
geometry matches the 600 dpi scan exactly (box 743.5 x 286 px vs
372 x 143 px at 600), and flat areas show no column banding (0.2 %).
Paper fibres are resolved.

## Known differences to watch

- Reset commands: HP's driver sends wIndex 0x0800/0x0801 with wLength 1;
  hp3900 sends 0x0801 with length 0 and 0x0800 with 2 bytes, and also uses
  0x0600 (DMA cancel), which HP's driver never issues. These worked on the
  first hardware run.
- Lamp stability: hp3900 compares readings against `Diff * 0.01`; with the
  profile's `Diff=100` that only passes on identical readings, so warm-up
  normally runs the full 10 s. Harmless, only slower.
- Black level: with fixed offsets and software white shading only, blacks may
  come out slightly gray or clipped if this unit's offsets differ much from
  the profile's typical values. The raw diagnostic scan shows it.
- NVRAM: the engine writes counters and the reference position (0x6a-0x7b)
  as the original driver does. `hp3770-probe --dump-nvram FILE` makes a
  read-only backup first.

## Verification done without hardware

- `tests/test_config.c` (`make check`): the HP3770 configuration resolves to
  the generated t6 tables (timings, 15 scan modes, 12 motor curves, motor
  moves, sensor/motor geometry, reference voltages, SER/LER, PWM, thresholds,
  calibration values, shading cut, lamp check, auto-reference) and not to
  HP3970 values.
- The complete scan pipeline runs against the simulator under
  AddressSanitizer and UndefinedBehaviorSanitizer: colour 75-1200 dpi, gray
  (both ways), lineart, 8/16-bit, raw (no shading), the HP3970 AFE block and
  the HP3970 profile.
- `hp3770-airscan` against the simulator with curl and with sane-airscan
  (see above), including an A4 page at 1200 dpi (446 MB peak memory).
- Register images compared with HP's driver as described above.

The simulator proves the software path and the register programming, not the
image quality: calibration and reference detection only see synthetic data.

## Provenance and licence

The backend is derived from SANE's hp3900 backend (GPL-2.0-or-later with the
SANE exception), so this project is GPL-2.0-or-later.

`backend/hp3770_legacy_tables.c` holds hardware parameters (sensor timings,
motor acceleration curves, scan modes, calibration starting values) that
`tools/gen_legacy_tables.py` extracted from the configuration data embedded in
HP's ScanJet 3770 Linux driver (`libsane-hp3770.so.1.0.13`), for
interoperability with the scanner. They are included so that the driver
builds without HP's file. HP's driver itself is not part of this project.
