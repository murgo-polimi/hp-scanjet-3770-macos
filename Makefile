# HP ScanJet 3770 driver for macOS
#
#   make              build hp3770-scan (the scanner tool), hp3770-airscan (the
#                     AirScan server for Image Capture, Preview and other Mac
#                     apps) and hp3770-probe
#   make check        configuration test + short simulated scan (no scanner needed)
#   make sane-backend experimental SANE backend (libsane-hp3770.1.so)
#   make tables       regenerate backend/hp3770_legacy_tables.c from HP's driver
#                     (python3; LEGACY_SO=path/to/libsane-hp3770.so.1.0.13)
#   make install      install hp3770-scan and hp3770-airscan into $(PREFIX)/bin
#   make install-service    start hp3770-airscan at login (LaunchAgent, run
#                           without sudo after make install)
#   make uninstall-service  stop it and remove the LaunchAgent
#
# Requirements: Xcode command line tools, Homebrew libusb and pkg-config
#   brew install libusb pkg-config

CC         ?= clang
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr/local
CFLAGS     ?= -O2 -g

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS   := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

WARN       := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
BACKEND_CF := -std=c11 $(WARN) -Icompat -DHP3770_CONFIG_DIR=\"$(PREFIX)/etc/sane.d\"

BUILD      := build
LEGACY_SO  := hp3770-legacy/contents/usr/lib/sane/libsane-hp3770.so.1.0.13

BACKEND_SRC := backend/hp3770.c backend/hp3770_sane.c backend/hp3770_rts8822.c \
               backend/hp3770_config.c backend/hp3770_legacy_tables.c \
               backend/hp3770_usb.c backend/hp3770_types.c backend/hp3770_debug.c
COMPAT_SRC  := compat/sanei_misc.c compat/sanei_constrain_value.c compat/sane_strstatus.c
COMPAT_HDR  := $(wildcard compat/sane/*.h)

all: hp3770-scan hp3770-airscan hp3770-probe

need-libusb:
	@if [ -z "$(LIBUSB_LIBS)" ]; then \
	  echo "libusb-1.0 was not found. Run: brew install libusb pkg-config"; exit 1; fi

$(BUILD):
	mkdir -p $(BUILD)

# ---------------------------------------------------------------- scanner tool

$(BUILD)/hp3770.o: $(BACKEND_SRC) $(COMPAT_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(BACKEND_CF) -c backend/hp3770.c -o $@

$(BUILD)/sanei_usb_libusb.o: compat/sanei_usb_libusb.c $(COMPAT_HDR) | need-libusb $(BUILD)
	$(CC) $(CFLAGS) $(BACKEND_CF) $(LIBUSB_CFLAGS) -c $< -o $@

$(BUILD)/%.o: compat/%.c $(COMPAT_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(BACKEND_CF) -c $< -o $@

FRONTEND_COMMON := frontend/hp3770_common.c frontend/hp3770_deripple.c frontend/hp3770_autowhite.c

$(BUILD)/hp3770_scan.o: frontend/hp3770_scan.c $(FRONTEND_COMMON) $(COMPAT_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(BACKEND_CF) -c $< -o $@

$(BUILD)/hp3770_airscan.o: frontend/hp3770_airscan.c frontend/hp3770_jpeg.c $(FRONTEND_COMMON) $(COMPAT_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(BACKEND_CF) -pthread -c $< -o $@

COMPAT_OBJ := $(BUILD)/sanei_misc.o $(BUILD)/sanei_constrain_value.o $(BUILD)/sane_strstatus.o

hp3770-scan: $(BUILD)/hp3770_scan.o $(BUILD)/hp3770.o $(BUILD)/sanei_usb_libusb.o $(COMPAT_OBJ) | need-libusb
	$(CC) $(CFLAGS) -o $@ $^ $(LIBUSB_LIBS) -lm

# ------------------------------------------- AirScan server for Mac apps

hp3770-airscan: $(BUILD)/hp3770_airscan.o $(BUILD)/hp3770.o $(BUILD)/sanei_usb_libusb.o $(COMPAT_OBJ) | need-libusb
	$(CC) $(CFLAGS) -pthread -o $@ $^ $(LIBUSB_LIBS) -lm

# ------------------------------------------------------- read-only USB probe

hp3770-probe: src/hp3770_probe.c src/hp3770_profile.c src/hp3770_profile.h | need-libusb
	$(CC) $(CFLAGS) -Wall -Wextra -Wpedantic -std=c17 $(LIBUSB_CFLAGS) -o $@ \
	  src/hp3770_probe.c src/hp3770_profile.c $(LIBUSB_LIBS)

# ------------------------------------------------- tests without hardware

$(BUILD)/test_config: tests/test_config.c tests/sanei_usb_mock.c $(BACKEND_SRC) $(COMPAT_SRC) $(COMPAT_HDR) | $(BUILD)
	$(CC) -O1 -g $(BACKEND_CF) -o $@ tests/test_config.c tests/sanei_usb_mock.c $(COMPAT_SRC) -lm

$(BUILD)/hp3770-scan-sim: frontend/hp3770_scan.c $(FRONTEND_COMMON) tests/sanei_usb_mock.c $(BACKEND_SRC) $(COMPAT_SRC) $(COMPAT_HDR) | $(BUILD)
	$(CC) -O1 -g $(BACKEND_CF) -o $@ frontend/hp3770_scan.c backend/hp3770.c \
	  tests/sanei_usb_mock.c $(COMPAT_SRC) -lm

# hp3770-airscan on the simulated scanner: make airscan-sim, then
#   HP3770_SIM_REGS=tests/idle-registers.bin build/hp3770-airscan-sim -v
$(BUILD)/hp3770-airscan-sim: frontend/hp3770_airscan.c frontend/hp3770_jpeg.c $(FRONTEND_COMMON) tests/sanei_usb_mock.c $(BACKEND_SRC) $(COMPAT_SRC) $(COMPAT_HDR) | $(BUILD)
	$(CC) -O1 -g $(BACKEND_CF) -pthread -o $@ frontend/hp3770_airscan.c backend/hp3770.c \
	  tests/sanei_usb_mock.c $(COMPAT_SRC) -lm

sim: $(BUILD)/hp3770-scan-sim
airscan-sim: $(BUILD)/hp3770-airscan-sim

check: $(BUILD)/test_config $(BUILD)/hp3770-scan-sim $(BUILD)/hp3770-airscan-sim
	$(BUILD)/test_config
	@echo "Simulated 100 dpi gray scan (about one minute: the backend waits for the lamp)..."
	HP3770_SIM_REGS=tests/idle-registers.bin $(BUILD)/hp3770-scan-sim -f -m gray -r 100 \
	  -x 40 -y 20 -o $(BUILD)/sim-check.tif
	@echo "check: OK"

# ------------------------------------------------- experimental SANE backend

sane-backend: $(BUILD)/libsane-hp3770.1.so

ifeq ($(shell uname),Darwin)
MODULE_LDFLAGS := -bundle
else
MODULE_LDFLAGS := -shared
BACKEND_CF += -fPIC
endif

$(BUILD)/libsane-hp3770.1.so: $(BUILD)/hp3770.o $(BUILD)/sanei_usb_libusb.o $(COMPAT_OBJ) | need-libusb
	$(CC) $(CFLAGS) $(MODULE_LDFLAGS) -o $@ $^ $(LIBUSB_LIBS) -lm

# ------------------------------------------------------------- maintenance

tables:
	python3 tools/gen_legacy_tables.py $(LEGACY_SO) backend/hp3770_config.c > backend/hp3770_legacy_tables.c.new
	mv backend/hp3770_legacy_tables.c.new backend/hp3770_legacy_tables.c

install: hp3770-scan hp3770-airscan
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 hp3770-scan $(DESTDIR)$(PREFIX)/bin/hp3770-scan
	install -m 755 hp3770-airscan $(DESTDIR)$(PREFIX)/bin/hp3770-airscan

# LaunchAgent: runs as you (not root), so run these without sudo
AGENT_LABEL := io.github.hp3770-macos.airscan
AGENT_PLIST := $(HOME)/Library/LaunchAgents/$(AGENT_LABEL).plist
AGENT_LOG   := $(HOME)/Library/Logs/hp3770-airscan.log

install-service: packaging/hp3770-airscan.plist.in
	@if [ "$$(id -u)" = 0 ]; then echo "Run make install-service without sudo."; exit 1; fi
	@if [ ! -x $(PREFIX)/bin/hp3770-airscan ]; then \
	  echo "$(PREFIX)/bin/hp3770-airscan not found: run 'sudo make install' first."; exit 1; fi
	mkdir -p $(HOME)/Library/LaunchAgents $(HOME)/Library/Logs
	sed -e 's|@LABEL@|$(AGENT_LABEL)|' -e 's|@BINDIR@|$(PREFIX)/bin|' -e 's|@LOGFILE@|$(AGENT_LOG)|' \
	  packaging/hp3770-airscan.plist.in > $(AGENT_PLIST)
	-launchctl bootout gui/$$(id -u)/$(AGENT_LABEL) 2>/dev/null
	launchctl bootstrap gui/$$(id -u) $(AGENT_PLIST)
	@echo "hp3770-airscan runs now and at every login. Log: $(AGENT_LOG)"

uninstall-service:
	-launchctl bootout gui/$$(id -u)/$(AGENT_LABEL)
	rm -f $(AGENT_PLIST)

clean:
	rm -rf $(BUILD) hp3770-scan hp3770-airscan hp3770-probe

.PHONY: all need-libusb sim airscan-sim check sane-backend tables install install-service \
        uninstall-service clean
