#!/bin/sh
# Build only. Never launches hardware, sysemu, or the device-management service.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
PLATFORM=${ET_PLATFORM_SOURCE:-$(dirname "$ROOT")/et-platform}
IMAGE=${ET_DOCKER_IMAGE:-et-soc1-dev:20260911}
sudo docker run --rm --user "$(id -u):$(id -g)" \
    -v "$ROOT:/work" -v "$PLATFORM:/src/et-platform:ro" -w /work "$IMAGE" \
    sh -c 'cmake -S et-kernels -B et-kernels/build-device \
       -DCMAKE_TOOLCHAIN_FILE=/opt/et/lib/cmake/riscv64-ec-toolchain.cmake \
       -DET_DEVICE=ON -DCMAKE_BUILD_TYPE=Release "$@" && \
       cmake --build et-kernels/build-device -j 8' sh "$@"
