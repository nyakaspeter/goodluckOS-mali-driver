#!/bin/sh
# Build glos-gpu and glos-gpu-switch for the console (armhf, Debian bookworm's
# cross compiler in the glos-kernel Docker image). Output next to the sources.
set -eu
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/helper:ro -v "$PWD":/out "${KERNEL_IMAGE:-glos-kernel:ready}" sh -c '
    cd /tmp && arm-linux-gnueabihf-gcc -O2 -Wall -o glos-gpu-switch /helper/glos-gpu-switch.c &&
    arm-linux-gnueabihf-gcc -O2 -Wall -o glos-gpu /helper/glos-gpu.c -lutil &&
    cat glos-gpu-switch > /out/glos-gpu-switch && cat glos-gpu > /out/glos-gpu'
ls -l glos-gpu glos-gpu-switch
