CC = gcc
COMMON = ../../../libretro-common
CFLAGS += -O2 -Wall -Wextra -std=c89 -pedantic -DHAVE_THREADS -I$(COMMON)/include
LDFLAGS += -lpthread
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -g -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif
SOURCES = pipeline_layout_test.c $(COMMON)/queues/retro_spsc.c $(COMMON)/rthreads/rthreads.c
all: pipeline_layout_test
pipeline_layout_test: $(SOURCES) ../../../audio/audio_pipeline_layout.h
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@
check: pipeline_layout_test
	./pipeline_layout_test
clean:
	rm -f pipeline_layout_test pipeline_layout_test.exe
.PHONY: all check clean
