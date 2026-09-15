#!/bin/sh
# Build only. Never launches hardware, sysemu, or the device-management service.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
PLATFORM=${ET_PLATFORM_SOURCE:-$(dirname "$ROOT")/et-platform}
IMAGE=${ET_DOCKER_IMAGE:-et-soc1-dev:20260911}
BUILD=${ET_KERNEL_BUILD:-et-kernels/build-device}
sudo docker run --rm --user "$(id -u):$(id -g)" \
    -v "$ROOT:/work" -v "$PLATFORM:/src/et-platform:ro" -w /work -e ET_KERNEL_BUILD="$BUILD" "$IMAGE" \
    sh -c 'cmake -S et-kernels -B "$ET_KERNEL_BUILD" \
       -DCMAKE_TOOLCHAIN_FILE=/opt/et/lib/cmake/riscv64-ec-toolchain.cmake \
       -DET_DEVICE=ON -DCMAKE_BUILD_TYPE=Release \
       -DET_CACHED_BITS=ON -DET_DIRECT_BITS=ON -DET_FAST_HEADERS=ON -DET_WIDE_MEM=ON \
       -DET_IDCT_SIMD=ON -DET_IDCT_ROWS=ON -DET_IDCT_PACKED=ON -DET_MC_IMPL=3 \
       -DET_MC_V8_STORE=0 -DET_SPREAD_HARTS=ON -DET_FAST_DC=ON -DET_OPT_LEVEL=2 \
       -DET_EVEN_ONLY=OFF -DET_PREQUANT=OFF -DET_PREFETCH_MC=OFF \
       -DET_BIT_WINDOW_BYTES=1024 "$@" && \
       cmake --build "$ET_KERNEL_BUILD" -j 8' sh "$@"
