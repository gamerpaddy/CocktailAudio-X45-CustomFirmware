# Cross-build for the Cocktail Audio X45 (i.MX6Q, armv7l hard-float).
#
# The device cannot link binaries: the vendor's image build ran a blanket strip
# that removed the symbol tables from /usr/lib/crt1.o, crti.o and Scrt1.o, so
# _start is gone and every native link dies. Everything is therefore
# cross-compiled here and linked FULLY STATIC against musl, which also means
# zero dependency on the device's eglibc 2.19 rootfs.
#
# Toolchain: Bootlin armv7-eabihf musl stable-2021.11 (scripts/toolchain.sh unpacks it).
# Normally you do not call make yourself: ./build.sh (scripts/panel.sh) runs it with the right settings.
#
#   make                 build the basic tools into out/
#   make player          build the player programs (needs the libraries from scripts/deps.sh)
#   make clean

TC      ?= /opt/x45tc
CROSS   ?= $(TC)/bin/arm-linux-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

# armv7-a + neon matches the i.MX6Q Cortex-A9. -static for the reason above.
ARCHFLAGS := -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
CFLAGS    := -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter $(ARCHFLAGS) \
             -ffunction-sections -fdata-sections
LDFLAGS   := -static -Wl,--gc-sections -Wl,-s

OUT ?= out
LIB := libx45/x45.c libx45/x45clock.c

# liba52 (a52dec 0.7.4, GPL), the Dolby Digital decoder of the video DVD player; see x45cd/a52/config.h
A52SRC := x45cd/a52/bit_allocate.c x45cd/a52/bitstream.c x45cd/a52/downmix.c x45cd/a52/imdct.c x45cd/a52/parse.c
A52HDR := $(wildcard x45cd/a52/*.h)

GFX := libx45/gfx.c libx45/font5x7.c libx45/textfold.c libx45/image.c

# Cross-built static dependencies (scripts/deps.sh puts them here).
DEPS    ?= $(HOME)/x45sysroot
DEPCFLAGS := -I$(DEPS)/include
# libFLAC needs libogg; alsa-lib needs -lm -lpthread -ldl even when static.
DEPLIBS   := -L$(DEPS)/lib -lFLAC -logg -lasound -lm -lpthread -ldl

BINS := $(OUT)/x45ctl $(OUT)/x45fb $(OUT)/x45in $(OUT)/x45ui $(OUT)/x45mpc

all: $(BINS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/x45ctl: x45ctl/main.c $(LIB) libx45/x45.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45ctl/main.c $(LIB) $(LDFLAGS)
	@echo "--- $@"
	@$(CROSS)readelf -h $@ | grep -E 'Class|Machine|Type'
	@ls -l $@

$(OUT)/x45fb: x45fb/main.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45fb/main.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45in: x45in/main.c $(LIB) libx45/x45in.c libx45/x45.h libx45/x45in.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45in/main.c $(LIB) libx45/x45in.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# The front-panel UI: software rendering on /dev/fb0, real input, real hardware.
# Screens that are not built into main.c live in x45ui/ext/ (see ext/ext.h); every *.c there is part of the UI.
# The same for the looks and the visuals: every x45ui/skins/*.c is a skin (see skins/skin.h), every x45ui/vis/*.c a visual (vis/vis.h).
EXT := $(wildcard x45ui/ext/*.c) $(wildcard x45ui/skins/*.c) $(wildcard x45ui/vis/*.c) libx45/wifi.c

$(OUT)/x45ui: x45ui/main.c x45ui/ui.h $(wildcard x45ui/ext/*.h) $(wildcard x45ui/skins/*.h) $(wildcard x45ui/vis/*.h) $(EXT) $(LIB) libx45/x45in.c libx45/mpdc.c libx45/stations.c $(GFX) \
              libx45/x45.h libx45/x45in.h libx45/gfx.h libx45/mpdc.h libx45/stations.h libx45/wifi.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45ui/main.c $(EXT) $(LIB) libx45/x45in.c libx45/mpdc.c libx45/stations.c $(GFX) $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# Pure-logic unit test for the quadrature decoder. Builds as ARM and runs on
# the device (it touches no hardware) since there is no native Linux gcc here.
$(OUT)/quad_test: tests/quad_test.c libx45/x45in.c libx45/x45in.h | $(OUT)
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ tests/quad_test.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45mpc: x45mpc/main.c libx45/mpdc.c libx45/mpdc.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45mpc/main.c libx45/mpdc.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# Protocol test against a forked fake MPD on an ephemeral localhost port.
$(OUT)/mpdc_test: tests/mpdc_test.c libx45/mpdc.c libx45/mpdc.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/mpdc_test.c libx45/mpdc.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# The same test built natively (WSL has a host gcc): quicker than a device round
# trip while iterating, and a second compiler/libc is a useful second opinion.
#
# `setarch -R` disables address-space randomisation for this one process. With
# gcc 9.4's ASan on a recent WSL2 kernel (6.18), even an EMPTY main() crashes
# about 1 run in 4 with AddressSanitizer:DEADLYSIGNAL while ASLR is on (22/80
# measured) and never with it off (0/80), so without this the test is flaky for
# reasons that have nothing to do with the code under test.
nativetest:
	mkdir -p $(OUT)
	gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	    -o $(OUT)/mpdc_test_native tests/mpdc_test.c libx45/mpdc.c
	setarch -R ./$(OUT)/mpdc_test_native
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/fold_test_native tests/fold_test.c libx45/textfold.c
	setarch -R ./$(OUT)/fold_test_native
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/stations_test_native tests/stations_test.c libx45/stations.c
	setarch -R ./$(OUT)/stations_test_native
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/srch_test_native tests/srch_test.c x45ui/ext/srchcore.c libx45/textfold.c
	setarch -R ./$(OUT)/srch_test_native
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/arb_test_native tests/arb_test.c libx45/x45arb.c
	setarch -R ./$(OUT)/arb_test_native
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/balance_test_native tests/balance_test.c libx45/x45.c
	setarch -R ./$(OUT)/balance_test_native
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/hdcd_test_native tests/hdcd_test.c x45cd/hdcd.c -lm
	setarch -R ./$(OUT)/hdcd_test_native
	for f in $(A52SRC); do gcc -std=gnu11 -O2 -w -c -o $(OUT)/a52_`basename $$f .c`.o $$f || exit 1; done
	gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
	    -o $(OUT)/dvd_test_native tests/dvd_test.c x45cd/dvd.c $(OUT)/a52_*.o -lm
	setarch -R ./$(OUT)/dvd_test_native
	sh tests/upnp_test.sh

# x45upnp against a fake media server on loopback (needs python3)
upnptest:
	sh tests/upnp_test.sh

$(OUT)/fold_test: tests/fold_test.c libx45/textfold.c libx45/gfx.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/fold_test.c libx45/textfold.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/stations_test: tests/stations_test.c libx45/stations.c libx45/stations.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/stations_test.c libx45/stations.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/balance_test: tests/balance_test.c $(LIB) libx45/x45.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/balance_test.c $(LIB) $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/clock_test: tests/clock_test.c libx45/x45clock.c libx45/x45.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/clock_test.c libx45/x45clock.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# The two pieces a program needs before it opens the DAC - pick the right master
# clock, and take the DAC from a live AirPlay session - as one small static
# library (scripts/mpd.sh links it into MPD). `make clocklib` installs it into the
# deps sysroot. (The name is historical; it holds both.)
$(OUT)/libx45clock.a: libx45/x45clock.c libx45/x45arb.c libx45/x45.h | $(OUT)
	$(CC) $(CFLAGS) -c -o $(OUT)/x45clock.o libx45/x45clock.c
	$(CC) $(CFLAGS) -c -o $(OUT)/x45arb.o libx45/x45arb.c
	$(CROSS)ar rcs $@ $(OUT)/x45clock.o $(OUT)/x45arb.o

$(OUT)/arb_test: tests/arb_test.c libx45/x45arb.c libx45/x45.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/arb_test.c libx45/x45arb.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

clocklib: $(OUT)/libx45clock.a
	install -d $(DEPS)/lib
	install -m644 $(OUT)/libx45clock.a $(DEPS)/lib/libx45clock.a

# Verifies the hand-authored 5x7 font table: mechanical self-check plus an
# ASCII-art render so the glyph shapes themselves are checkable.
$(OUT)/font_test: tests/font_test.c libx45/font5x7.c libx45/gfx.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/font_test.c libx45/font5x7.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# The player. Needs the cross-built static deps; run scripts/deps.sh first.
# Kept out of `all` so the core tools still build without them.
$(OUT)/x45play: x45play/main.c $(LIB) libx45/x45.h | $(OUT)
	@test -f $(DEPS)/lib/libFLAC.a || \
	  { echo "missing $(DEPS)/lib/libFLAC.a - run scripts/deps.sh first"; exit 1; }
	$(CC) $(CFLAGS) $(DEPCFLAGS) -o $@ x45play/main.c $(LIB) \
	      $(DEPLIBS) $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45loop: x45loop/main.c $(LIB) libx45/x45.h | $(OUT)
	@test -f $(DEPS)/lib/libasound.a || \
	  { echo "missing $(DEPS)/lib/libasound.a - run scripts/deps.sh first"; exit 1; }
	$(CC) $(CFLAGS) $(DEPCFLAGS) -o $@ x45loop/main.c $(LIB) \
	      -L$(DEPS)/lib -lasound -lm -lpthread -ldl $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45cd: x45cd/main.c x45cd/hdcd.c x45cd/hdcd.h x45cd/dvd.c x45cd/dvd.h $(A52SRC) $(A52HDR) $(LIB) libx45/x45.h | $(OUT)
	@test -f $(DEPS)/lib/libasound.a || \
	  { echo "missing $(DEPS)/lib/libasound.a - run scripts/deps.sh first"; exit 1; }
	$(CC) $(CFLAGS) $(DEPCFLAGS) -o $@ x45cd/main.c x45cd/hdcd.c x45cd/dvd.c $(A52SRC) $(LIB) \
	      -L$(DEPS)/lib -lasound -lm -lpthread -ldl $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45web: x45web/main.c x45web/web.h x45web/api_*.c x45web/libsync.c libx45/wifi.c libx45/wifi.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45web/main.c $(wildcard x45web/api_*.c x45web/libsync.c) libx45/wifi.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45rec: x45rec/main.c x45cd/dvd.c x45cd/dvd.h $(A52SRC) $(A52HDR) | $(OUT)
	@test -f $(DEPS)/lib/libFLAC.a -a -f $(DEPS)/lib/libmp3lame.a || 	  { echo "missing libFLAC.a or libmp3lame.a in $(DEPS)/lib - run scripts/deps.sh first"; exit 1; }
	$(CC) $(CFLAGS) $(DEPCFLAGS) -o $@ x45rec/main.c x45cd/dvd.c $(A52SRC) -L$(DEPS)/lib -lmp3lame $(DEPLIBS) $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45tuner: x45tuner/main.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45tuner/main.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45upnp: x45upnp/main.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45upnp/main.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

$(OUT)/x45rb: x45rb/main.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45rb/main.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# The artwork finder (what is playing -> a picture): runs curl, speaks MPD's picture commands itself.
$(OUT)/x45art: x45art/main.c libx45/stb_image.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45art/main.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# WiFi: the client library (talks to wpa_supplicant) and its command line.
$(OUT)/x45wifi: x45wifi/main.c libx45/wifi.c libx45/wifi.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ x45wifi/main.c libx45/wifi.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# The remote-control logger names the key codes with the panel's own table.
$(OUT)/x45irtest: tools/x45irtest.c libx45/x45in.c libx45/x45in.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/x45irtest.c libx45/x45in.c $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

# Maintenance tools (tools/): small stand-alone helpers, not part of the stack.
$(OUT)/%: tools/%.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "--- $@"
	@ls -l $@

player: $(OUT)/x45play $(OUT)/x45loop $(OUT)/x45cd $(OUT)/x45tuner $(OUT)/x45rec $(OUT)/x45web $(OUT)/x45upnp $(OUT)/x45rb $(OUT)/x45wifi $(OUT)/x45art

test: $(OUT)/quad_test $(OUT)/font_test $(OUT)/clock_test $(OUT)/mpdc_test $(OUT)/fold_test $(OUT)/stations_test $(OUT)/arb_test $(OUT)/balance_test

clean:
	rm -rf $(OUT)

.PHONY: all clean test player clocklib nativetest upnptest
