# FFMPEG_BUILD is the independently configured CPU build supplied by the parent.
FFMPEG_BUILD ?= ../../../build-et/aac-prototype/cpu
FFMPEG_SRC ?= ../../..
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I.. -I$(FFMPEG_BUILD) -I$(FFMPEG_SRC)
CFLAGS += -std=c11 -Wall -Wextra -Werror -ffp-contract=off
LDLIBS += $(FFMPEG_BUILD)/libavcodec/libavcodec.a $(FFMPEG_BUILD)/libavutil/libavutil.a -lm -pthread

all: test_etaac_ffmpeg

test_etaac_ffmpeg: test_etaac_ffmpeg.c ../etaac_synth.c ../etaac_tables.h ../synth.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_etaac_ffmpeg.c ../etaac_synth.c $(LDLIBS)

run: test_etaac_ffmpeg
	./test_etaac_ffmpeg

clean:
	rm -f test_etaac_ffmpeg
