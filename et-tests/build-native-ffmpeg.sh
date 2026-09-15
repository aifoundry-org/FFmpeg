#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?Usage: build-native-ffmpeg.sh OUTPUT_DIR [NATIVE_KERNEL_LIB]}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
"$ROOT/et-tests/build-native-runtime.sh" "$OUT/runtime" "${2:-}"
mkdir -p "$OUT/ffmpeg"
cd "$OUT/ffmpeg"
echo '*** CONFIGURING FFmpeg NATIVE TEST — NO HARDWARE VALIDATION ***' >&2
PKG_CONFIG_PATH="$OUT/runtime/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
"$ROOT/configure" --disable-autodetect --disable-x86asm --disable-doc --disable-debug \
    --disable-everything --disable-ffprobe --enable-ffmpeg --enable-etsoc --enable-hwaccel=mpeg2_et --enable-avdevice --enable-avfilter \
    --enable-protocol=file --enable-indev=lavfi --enable-filter=testsrc2,format,scale \
    --enable-decoder=mpeg2video,rawvideo,wrapped_avframe --enable-encoder=mpeg1video,mpeg2video,rawvideo \
    --enable-parser=mpegvideo --enable-demuxer=mpegvideo,rawvideo \
    --enable-muxer=mpeg1video,mpeg2video,framemd5,rawvideo,null
make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
# FFmpeg's makefiles do not track external static archive contents. Always
# relink after rebuilding the native runtime/kernel, even with unchanged config.
make -W fftools/ffmpeg.o ffmpeg
printf '\nNATIVE TEST ffmpeg: %s/ffmpeg/ffmpeg\n' "$OUT"
