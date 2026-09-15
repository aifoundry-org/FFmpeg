#!/bin/sh
# Standalone compile/native tests only. NO ET execution, hardware or emulator.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="$ROOT/build-et/motion-probes"
mkdir -p "$OUT"
CC=${CC:-cc}
FLAGS='-std=c11 -O2 -g -Wall -Wextra -Werror -UNDEBUG'
for load in 0 1; do
    for xy in 0 1; do
        "$CC" $FLAGS -DET_MC_LOAD=$load -DET_MC_XY=$xy \
            "$ROOT/et-kernels/tests/test_motion.c" -o "$OUT/native-$load-$xy"
        "$OUT/native-$load-$xy"
        "$CC" $FLAGS -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
            -DET_MC_LOAD=$load -DET_MC_XY=$xy \
            "$ROOT/et-kernels/tests/test_motion.c" -o "$OUT/sanitize-$load-$xy"
        "$OUT/sanitize-$load-$xy"
    done
done
"$CC" $FLAGS -DET_MC_IMPL=0 "$ROOT/et-kernels/tests/test_motion.c" -o "$OUT/native-scalar"
"$OUT/native-scalar"
python3 "$ROOT/et-kernels/tests/test_motion_vector.py"

if [ "${1:-}" = --device-compile ]; then
    cat > "$OUT/compile.c" <<'EOF'
#include "et-kernels/src/motion.h"
void motion_runtime(uint8_t *d, const uint8_t *s, size_t pitch, unsigned n,
                    unsigned x, unsigned y, unsigned average)
{ et_mc_predict(d,s,pitch,n,x,y,average); }
void motion_xy16(uint8_t *d, const uint8_t *s, size_t pitch)
{ et_mc_predict(d,s,pitch,16,1,1,1); }
EOF
    "$ROOT/et-tools/et-env" sh -c '
        set -eu
        OUT=build-et/motion-probes
        CC=/opt/et/bin/riscv64-unknown-elf-gcc
        for impl in 0 1 2 3 4; do
            for load in 0 1; do
                for xy in 0 1; do
                    stem=impl$impl-load$load-xy$xy
                    "$CC" -I. -DET_DEVICE=1 -DET_MC_IMPL=$impl \
                        -DET_MC_LOAD=$load -DET_MC_XY=$xy -O2 \
                        -ffreestanding -fno-builtin -mstrict-align \
                        -march=rv64imf -mabi=lp64f -mcmodel=medany \
                        -fno-tree-vectorize -Wall -Wextra -Werror \
                        -S "$OUT/compile.c" -o "$OUT/$stem.s"
                    "$CC" -march=rv64imf -mabi=lp64f -c "$OUT/$stem.s" -o "$OUT/$stem.o"
                    /opt/et/bin/riscv64-unknown-elf-objdump -dr "$OUT/$stem.o" > "$OUT/$stem.dis"
                    /opt/et/bin/riscv64-unknown-elf-size "$OUT/$stem.o"
                done
            done
        done
        stem=impl3-v8store1
        "$CC" -I. -DET_DEVICE=1 -DET_MC_IMPL=3 -DET_MC_V8_STORE=1 -O2 \
            -ffreestanding -fno-builtin -mstrict-align \
            -march=rv64imf -mabi=lp64f -mcmodel=medany \
            -fno-tree-vectorize -Wall -Wextra -Werror \
            -S "$OUT/compile.c" -o "$OUT/$stem.s"
        "$CC" -march=rv64imf -mabi=lp64f -c "$OUT/$stem.s" -o "$OUT/$stem.o"
        /opt/et/bin/riscv64-unknown-elf-objdump -dr "$OUT/$stem.o" > "$OUT/$stem.dis"
        /opt/et/bin/riscv64-unknown-elf-size "$OUT/$stem.o"
        for impl in 0 1 2 3 4; do
            test -z "$(/opt/et/bin/riscv64-unknown-elf-nm -u "$OUT/impl$impl-load1-xy1.o")"
        done
        if grep -E "fsw[lg]\\.ps|fsc[bhw][lg]\\.ps" "$OUT"/impl3-*.dis "$OUT"/impl4-*.dis; then
            echo "Forbidden cache-bypassing motion store" >&2; exit 1
        fi'
fi
