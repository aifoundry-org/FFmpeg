#!/bin/sh
# Run from the repository root; no PCIe nodes are exposed to this container.
set -eu
sudo docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e FF_ET_SYSEMU=1 -e FF_ET_MEM_CHECK=1 \
    -v "$PWD:/work" -v "$PWD/../et-platform:/src/et-platform:ro" -w /work \
    et-soc1-dev:20260911 timeout 900 ./et-runtime/build/et-runtime-smoke --dma
