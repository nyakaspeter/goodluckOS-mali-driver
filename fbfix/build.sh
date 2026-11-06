#!/bin/sh
# Build glos_fbfix.ko (and the fbinfo probe) against a goodluckOS kernel tree
# in Docker. KERNEL_IMAGE: glos-kernel:ready (stock config, default) or
# glos-kernel:mali-ready (the rebuilt kernel's config). Output in out/.
set -eu
cd "$(dirname "$0")"
mkdir -p out
docker run --rm -v "$PWD":/fbfix:ro -v "$PWD/out":/out "${KERNEL_IMAGE:-glos-kernel:ready}" sh -c '
    cp -r /fbfix /tmp/fbfix && rm -rf /tmp/fbfix/out &&
    make -C /src/linux M=/tmp/fbfix ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules > /tmp/build.log 2>&1 &&
    arm-linux-gnueabihf-gcc -O2 -static -o /tmp/fbinfo /tmp/fbfix/fbinfo.c >> /tmp/build.log 2>&1;
    rc=$?; cat /tmp/build.log > /out/build.log; cat /tmp/fbfix/glos_fbfix.ko > /out/glos_fbfix.ko 2>/dev/null; cat /tmp/fbinfo > /out/fbinfo 2>/dev/null; exit $rc'
echo "built out/glos_fbfix.ko ($(stat -f %z out/glos_fbfix.ko) bytes), out/fbinfo"
