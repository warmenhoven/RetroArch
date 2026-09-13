CC = gcc
ROOT = ../../..
COMMON = $(ROOT)/libretro-common
CPPFLAGS += -DRARCH_INTERNAL -I$(ROOT) -I$(COMMON)/include -I$(ROOT)/deps -I$(ROOT)/deps/stb
CFLAGS += -O2 -std=gnu99 -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
LDLIBS += -lm
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=$(SANITIZER)
endif
SOURCES = $(COMMON)/audio/resampler/drivers/sinc_resampler.c \
 $(COMMON)/audio/resampler/drivers/sinc_resampler_int16.c \
 $(COMMON)/audio/conversion/s16_to_float.c $(COMMON)/audio/conversion/float_to_s16.c \
 $(COMMON)/memmap/memalign.c
all: extra_capacity_test
# Internalize unrelated frontend entry points in this focused include harness.
extra_capacity_test.o: extra_capacity_test.c $(ROOT)/audio/audio_driver.c $(ROOT)/audio/audio_driver.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fwhole-program -c $< -o $@
extra_capacity_test: extra_capacity_test.o $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
check: extra_capacity_test
	./extra_capacity_test
clean:
	rm -f extra_capacity_test extra_capacity_test.exe extra_capacity_test.o
.PHONY: all check clean
