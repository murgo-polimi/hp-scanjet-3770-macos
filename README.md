# HP ScanJet 3770 driver for macOS

Use an **HP ScanJet 3770** (USB `03f0:2505`) on a current Mac: from Image
Capture, Preview and other Mac apps, or from the command line. HP never
shipped a driver for modern macOS; this one runs entirely in user space (no
kernel extension, no system changes) and talks to the scanner through libusb.

- **Mac apps**: `hp3770-airscan` presents the scanner to macOS as an AirScan
  (eSCL) scanner, which Image Capture, Preview ("Import from Scanner") and
  any app using Apple's scanner framework support without a driver. By
  default only your own Mac can see it.
- **Command line**: `hp3770-scan` writes TIFF or PNM, 8 or 16 bits.
- Colour, gray and lineart at 50-1200 dpi, A4/Letter flatbed.
- Image corrections HP's driver lacked: removal of the faint colour bands
  caused by the lamp's mains ripple, and an automatic paper white point.

Tested on macOS Tahoe with Apple silicon. The engine is SANE's `hp3900`
backend (same RTS8822 controller) with a dedicated HP3770 profile, checked
register by register against HP's own Linux driver. Details:
[`docs/porting-notes.md`](docs/porting-notes.md).

Only the 3770 is detected and supported. The backend still carries the
profiles of the other hp3900-family scanners (ScanJet 3800/3970/4070/4370,
G2710, G3010, G3110), but they are untested with the changes made here; for
those, use SANE's own `hp3900` backend.

## Build

```sh
xcode-select --install          # Apple's command-line tools, if missing
brew install libusb pkg-config  # Homebrew: https://brew.sh
git clone https://github.com/marcello-mu/hp-scanjet-3770-macos.git
cd hp-scanjet-3770-macos
make                            # hp3770-scan, hp3770-airscan, hp3770-probe
make check                      # optional: tests with a simulated scanner (~1 min)
```

Connect the scanner directly to the Mac (not through an unpowered hub) and
switch it on.

## Scanning from Mac apps

Start the server in a Terminal window and keep the window open:

```sh
./hp3770-airscan
```

Open **Image Capture**: "HP ScanJet 3770" appears in the sidebar. Select it;
the first overview scan takes about a minute (lamp warm-up and calibration).
Choose the area, resolution (75-1200 dpi), colour or gray, and JPEG or PDF,
then Scan. In **Preview**, use File > Import from Scanner. Stop the server
with Ctrl-C; a scan in progress finishes first and the head is parked.

To start it automatically at every login:

```sh
sudo make install               # hp3770-scan and hp3770-airscan into /usr/local/bin
make install-service            # without sudo: a LaunchAgent for your user
```

The log is `~/Library/Logs/hp3770-airscan.log`; `make uninstall-service`
removes the LaunchAgent. `http://localhost:8770/` shows the server status.

Options: `--port N` (default 8770), `--name "Office scanner"`,
`--quality 1..100` (JPEG, default 90), `-v` (log every request),
`--share` (offer the scanner to other computers on the local network),
`--announce-network` (announce it on the network but still accept this Mac
only).

The server opens the scanner only while it scans, so `hp3770-scan` keeps
working alongside it, just not at the same moment.

## Command line

```sh
./hp3770-scan --list                          # finds the scanner, nothing moves
./hp3770-scan -m color -r 300 --a4 -o page.tif
./hp3770-scan -m gray -r 600 -l 20 -t 30 -x 100 -y 70 -o detail.tif
```

```
hp3770-scan [options] -o FILE

  -o FILE              output: .tif/.tiff (default) or .pnm/.ppm/.pgm/.pbm
  -m color|gray|lineart
  -r DPI               50 75 100 150 200 300 600 1200 (default 300)
  -d 8|16              bits per channel
  -l MM -t MM          top-left corner (default 0, 0)
  -x MM -y MM          width and height (default: whole glass, 220 x 300 mm)
  --a4 | --letter      preset area
  --set NAME=VALUE     any backend option (see -A)
  -A                   list all backend options and their values
  --info               open the scanner and print chip and sensor details
  -f                   overwrite the output file
  --no-deripple        keep the lamp ripple bands
  --no-auto-white      no paper white point (keep calibrated levels)
  -v                   verbose
```

The first scan after switching on takes up to two minutes (lamp warm-up,
reference search, calibration); later ones about 30-45 s plus the scan
itself. Convert with macOS's own tools, e.g.
`sips -s format jpeg page.tif --out page.jpg`.

### Image corrections

The 3770's lamp flickers slightly at twice the mains frequency, which shows
as faint horizontal colour bands at 100-300 dpi. After each scan the bands
are measured and divided out (`-v` reports it, `--no-deripple` keeps the raw
image, `HP3770_MAINS=50|60` fixes the mains frequency).

The scanner calibrates against a strip that is whiter than most paper, so
document backgrounds would come out light grey-blue. If the image has a clear
paper background, its white is mapped to a neutral 245 of 255
(`--no-auto-white` keeps the calibrated levels, `HP3770_WHITE_TARGET=200..255`
changes the target). Photos without a white background are left alone.

### 1200 dpi

1200 dpi is the sensor's full resolution, for photos and small originals. It
takes twice as long as 600 dpi: about 45 s for a 10 x 15 cm photo after
warm-up, a minute and a half for a full A4 page, which is some 400 MB in
memory and as TIFF (800 MB at 16 bits). Scan only the area you need.

## Troubleshooting

| Symptom | Try |
| --- | --- |
| `no HP ScanJet 3770 found` | connect it directly (no hub), switch it on, check `system_profiler SPUSBDataType` |
| `cannot claim interface` | another program is scanning (hp3770-airscan, another hp3770-scan): wait, or unplug and replug |
| the scanner does not appear in Image Capture | check that `hp3770-airscan` runs (`http://localhost:8770/` answers), restart Image Capture, or try `--announce-network` |
| an app reports a scan error | run `hp3770-airscan -v` in Terminal and read its messages; `hp3770-scan` with the same settings shows scanner problems directly |
| image shifted vertically, or a dark band at the top | adjust `HP3770_AUTOREF_Y` (default 650, in 1/2400 inch) |
| a pale rectangle in the middle of A4 scans | the film window in the 3770's lid showing through thin paper: put a white sheet behind the page |

`SANE_DEBUG_HP3770=1..4` prints backend debug output (4 = every register
access); `HP3770_USB_TRACE=file` writes a compact log of every USB transfer.
Please attach both when reporting a problem.

Before experimenting, `./hp3770-probe --dump-nvram nvram.bin` saves a
read-only backup of the scanner's small NVRAM, where the driver (like HP's)
stores calibration counters and the reference position.

## Other tools

- `hp3770-probe`: read-only diagnostics (`--claim`, `--revision`,
  `--snapshot-registers FILE`, `--dump-nvram FILE`); never moves the head.
- `tools/diag-scans.sh`: a series of test scans (colour 100/300/600 dpi, gray
  both ways, raw data without shading) into a `diag-<date>` folder.
- `make sane-backend`: experimental `build/libsane-hp3770.1.so` for a
  Homebrew `sane-backends` install. Untested; `hp3770-scan` and
  `hp3770-airscan` are the supported paths.
- `make tables`: regenerates `backend/hp3770_legacy_tables.c` from HP's
  driver (`LEGACY_SO=path/to/libsane-hp3770.so.1.0.13`).

## Layout

```
backend/    hp3770 SANE backend (hp3900 engine + HP3770 profile and fixes)
compat/     sanei layer on libusb-1.0 (replaces a SANE installation)
frontend/   hp3770-scan, hp3770-airscan (eSCL server, JPEG/PDF writer) and
            the shared image corrections
packaging/  LaunchAgent template for hp3770-airscan
tests/      configuration test and simulated scanner
tools/      table generator, diagnostic scan scripts
src/        hp3770-probe
docs/       porting notes (how the driver was built and verified) and the
            diff against SANE's hp3900 backend
```

## Licence and credits

GPL-2.0-or-later (see [`LICENSE`](LICENSE)). The backend derives from the
SANE `hp3900` backend by Jonathan Bravo Lopez, from sane-backends 1.2.1; its
files keep their original copyright notices and the SANE linking exception.

`backend/hp3770_legacy_tables.c` contains hardware parameters (sensor
timings, motor curves, scan modes, calibration starting values) extracted
from the configuration data in HP's ScanJet 3770 Linux driver for
interoperability; see [`docs/porting-notes.md`](docs/porting-notes.md).
HP's driver itself is not included.

HP and ScanJet are trademarks of HP Inc. This project is not affiliated with
or endorsed by HP.
