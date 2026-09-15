#!/usr/bin/env bash
# Gate: --disable-etsoc must neither probe nor link any ET SDK/test dependency.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?Usage: check-cpu-disabled.sh OUTPUT_DIR}
mkdir -p "$OUT/empty-pkgconfig" "$OUT/ffmpeg"; OUT=$(cd "$OUT" && pwd)
export ET_TEST_REAL_PKG_CONFIG=$(command -v pkg-config)
export ET_TEST_PKG_LOG="$OUT/pkg-config.log"
: > "$ET_TEST_PKG_LOG"
cat > "$OUT/no-etsoc-pkg-config" <<'PC'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$ET_TEST_PKG_LOG"
case "$*" in *etsoc*) echo 'FAIL: ET-disabled configure probed etsoc' >&2; exit 99 ;; esac
exec "$ET_TEST_REAL_PKG_CONFIG" "$@"
PC
chmod +x "$OUT/no-etsoc-pkg-config"
# Isolate the disabled build from every installed etsoc.pc, including the mock.
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$OUT/empty-pkgconfig"
unset FF_ET_KERNEL FF_ET_SYSEMU FF_ET_MEM_CHECK FF_ET_TEST_FAIL
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS LIBRARY_PATH LD_LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
cd "$OUT/ffmpeg"
echo 'CPU-ONLY DISABLED GATE: no ET SDK, native runtime, simulator, or hardware dependency.' >&2
"$ROOT/configure" --disable-autodetect --disable-x86asm --disable-doc --disable-debug \
    --disable-everything --disable-ffprobe --enable-ffmpeg --disable-etsoc \
    --pkg-config="$OUT/no-etsoc-pkg-config" --enable-avdevice --enable-avfilter \
    --enable-protocol=file --enable-indev=lavfi --enable-filter=testsrc2,format,scale \
    --enable-decoder=mpeg2video,rawvideo,wrapped_avframe --enable-encoder=mpeg2video,rawvideo \
    --enable-parser=mpegvideo --enable-demuxer=mpegvideo,rawvideo \
    --enable-muxer=mpeg2video,framemd5,rawvideo,null
if grep -q etsoc "$ET_TEST_PKG_LOG"; then echo 'FAIL: etsoc pkg-config probe' >&2; exit 1; fi
grep -qx '#define CONFIG_ETSOC 0' config.h
grep -qx '#define CONFIG_MPEG2_ET_HWACCEL 0' config_components.h
make -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
if nm libavcodec/libavcodec.a 2>/dev/null | grep -E 'ff_et_runtime_|et_mpeg2_decode' > "$OUT/et-symbols.log"; then
    echo 'FAIL: ET runtime/kernel symbols remain in ET-disabled libavcodec' >&2; exit 1
fi
ldd ./ffmpeg > "$OUT/ldd.log"
if grep -Ei 'etsoc|et_mpeg2|libetrt|libdeviceLayer|libsw-sysemu' "$OUT/ldd.log"; then
    echo 'FAIL: ET-disabled FFmpeg links an ET dependency' >&2; exit 1
fi
./ffmpeg -hide_banner -hwaccels > "$OUT/hwaccels.log" 2>&1
if grep -qx et "$OUT/hwaccels.log"; then echo 'FAIL: ET accel still advertised' >&2; exit 1; fi
./ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=64x48:rate=25 \
    -frames:v 1 -c:v mpeg2video -threads 1 -g 1 -pix_fmt yuv420p -f mpeg2video "$OUT/single-i.m2v"
for idct in default simple; do
    args=(); [[ $idct == default ]] || args=(-idct simple)
    ./ffmpeg -nostdin -hide_banner -loglevel error -y -xerror -err_detect explode "${args[@]}" \
        -i "$OUT/single-i.m2v" -pix_fmt yuv420p -f framemd5 "$OUT/cpu-$idct.md5"
    [[ $(grep -c '^0,' "$OUT/cpu-$idct.md5") == 1 ]]
done
cmp "$OUT/cpu-default.md5" "$OUT/cpu-simple.md5"
echo 'PASS: CPU-only --disable-etsoc build and decode; zero ET pkg-config probes, symbols, or shared dependencies.'
