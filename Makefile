CC ?= cc
CFLAGS ?= -Os -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections
CPPFLAGS += -Iinclude -Igenerated
LDFLAGS += -Wl,--gc-sections
SOURCES := $(wildcard src/*.c)
CORE := $(filter-out src/main.c src/menu.c src/preset_menu.c,$(SOURCES))

.PHONY: all clean test
all: build/capsule-tweaker
build:
	mkdir -p build
build/capsule-tweaker: $(SOURCES) include/capsule.h generated/ring_palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/capsule-tweaker-test: $(SOURCES) include/capsule.h generated/ring_palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTESTING $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/test-core: tests/test_native.c $(CORE) include/capsule.h generated/ring_palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTESTING tests/test_native.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/test-audio: tests/test_audio_ioctl.c src/audio.c src/util.c src/config.c src/presets.c include/capsule.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_audio_ioctl.c src/audio.c src/util.c src/config.c src/presets.c $(LDFLAGS) -Wl,--wrap=open,--wrap=close,--wrap=ioctl -o $@
build/test-presets: tests/test_presets.c $(CORE) include/capsule.h generated/ring_palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTESTING tests/test_presets.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/test-display: tests/test_display.c $(CORE) include/capsule.h generated/ring_palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTESTING tests/test_display.c $(CORE) $(LDFLAGS) $(LDLIBS) -Wl,--wrap=write -o $@
test: build/test-core build/test-audio build/test-presets build/test-display build/capsule-tweaker-test
	./build/test-core
	./build/test-audio
	./build/test-presets
	./build/test-display
	python3 tests/integration_native.py ./build/capsule-tweaker-test
clean:
	rm -rf build
