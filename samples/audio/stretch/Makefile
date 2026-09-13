CC = gcc
ROOT = ../../..
CPPFLAGS += -I$(ROOT)/libretro-common/include
CFLAGS += -O2 -std=c89 -Wall -Wextra
WRAP_LDFLAGS = -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
LDLIBS += -lm
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=$(SANITIZER)
endif
SOURCES = stretch_test.c $(ROOT)/audio/audio_stretch.c
all: stretch_test stretch_src_test stretch_search_test stretch_canonical_test stretch_epoch_test stretch_pipeline_test
stretch_test: $(SOURCES) $(ROOT)/audio/audio_stretch.h $(ROOT)/libretro-common/include/audio/wsola_search.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(WRAP_LDFLAGS) $(LDLIBS) -o $@
stretch_src_test: stretch_src_test.c $(SOURCES) $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler_int16.c $(ROOT)/libretro-common/memmap/memalign.c
	$(CC) $(CPPFLAGS) $(CFLAGS) stretch_src_test.c $(ROOT)/audio/audio_stretch.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler_int16.c $(ROOT)/libretro-common/memmap/memalign.c $(LDFLAGS) $(WRAP_LDFLAGS) $(LDLIBS) -o $@
stretch_search_test: stretch_search_test.c $(ROOT)/audio/audio_stretch.c $(ROOT)/audio/audio_stretch.h $(ROOT)/libretro-common/include/audio/wsola_search.h
	$(CC) $(CPPFLAGS) $(CFLAGS) stretch_search_test.c $(LDFLAGS) $(LDLIBS) -o $@
stretch_canonical_test: stretch_canonical_test.c $(SOURCES) $(ROOT)/audio/audio_stretch.h
	$(CC) $(CPPFLAGS) $(CFLAGS) stretch_canonical_test.c $(ROOT)/audio/audio_stretch.c $(LDFLAGS) $(WRAP_LDFLAGS) $(LDLIBS) -o $@
stretch_epoch_test: stretch_epoch_test.c stretch_src_test.c $(SOURCES) $(ROOT)/audio/audio_stretch.h $(ROOT)/audio/audio_pipeline_layout.h $(ROOT)/libretro-common/queues/retro_spsc.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler_int16.c $(ROOT)/libretro-common/memmap/memalign.c
	$(CC) $(CPPFLAGS) $(CFLAGS) stretch_epoch_test.c $(ROOT)/audio/audio_stretch.c $(ROOT)/libretro-common/queues/retro_spsc.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler_int16.c $(ROOT)/libretro-common/memmap/memalign.c $(LDFLAGS) $(WRAP_LDFLAGS) $(LDLIBS) -o $@
stretch_pipeline_test: stretch_pipeline_test.c stretch_epoch_test.c stretch_src_test.c $(SOURCES) $(ROOT)/audio/audio_stretch.h $(ROOT)/audio/audio_pipeline_stretch.c $(ROOT)/audio/audio_pipeline_stretch.h $(ROOT)/audio/audio_pipeline_layout.h $(ROOT)/libretro-common/queues/retro_spsc.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler_int16.c $(ROOT)/libretro-common/memmap/memalign.c $(ROOT)/libretro-common/rthreads/rthreads.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHAVE_THREADS stretch_pipeline_test.c $(ROOT)/audio/audio_stretch.c $(ROOT)/audio/audio_pipeline_stretch.c $(ROOT)/libretro-common/queues/retro_spsc.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler.c $(ROOT)/libretro-common/audio/resampler/drivers/sinc_resampler_int16.c $(ROOT)/libretro-common/memmap/memalign.c $(ROOT)/libretro-common/rthreads/rthreads.c $(LDFLAGS) -lpthread $(WRAP_LDFLAGS) $(LDLIBS) -o $@
stretch_bench: stretch_bench.c $(ROOT)/audio/audio_stretch.c $(ROOT)/audio/audio_stretch.h $(ROOT)/libretro-common/include/audio/wsola_search.h
	$(CC) $(CPPFLAGS) $(CFLAGS) stretch_bench.c $(ROOT)/audio/audio_stretch.c $(LDFLAGS) $(LDLIBS) -o $@
bench: stretch_bench
	./stretch_bench
check: stretch_test stretch_src_test stretch_search_test stretch_canonical_test stretch_epoch_test stretch_pipeline_test
	./stretch_test
	./stretch_src_test
	./stretch_search_test
	./stretch_canonical_test
	./stretch_epoch_test
	./stretch_pipeline_test
clean:
	rm -f stretch_test stretch_test.exe stretch_src_test stretch_src_test.exe stretch_search_test stretch_search_test.exe stretch_bench stretch_bench.exe stretch_canonical_test stretch_canonical_test.exe stretch_epoch_test stretch_epoch_test.exe stretch_pipeline_test stretch_pipeline_test.exe
.PHONY: all check clean bench
