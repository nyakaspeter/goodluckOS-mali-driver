#!/bin/sh
# Build mali.ko against the stock goodluckOS kernel tree (Docker image
# glos-kernel:ready, made by make-kernel-image.sh).
# Usage: ./build-module.sh   -> out/mali.ko, build log in out/build.log
set -eu
cd "$(dirname "$0")"
mkdir -p out
rm -f out/mali.ko
docker run --rm -v "$PWD/driver":/mali:ro -v "$PWD/out":/out ${KERNEL_IMAGE:-glos-kernel:ready} sh -c '
    cp -r /mali /tmp/mali &&
    make -C /tmp/mali JOBS=$(nproc) KDIR=/src/linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
        USING_UMP=0 BUILD=release USING_PROFILING=0 MALI_PLATFORM=sunxi \
        USING_DVFS=0 USING_DEVFREQ=0 > /out/build.log 2>&1;
    rc=$?; cp -f /tmp/mali/mali.ko /out/ 2>/dev/null; exit $rc'
