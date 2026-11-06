#!/bin/sh
# Build the helpers and the fix module and refresh glos/, the driver set every
# game folder carries (copied into ports/<game>/glos/ by each port's
# package.sh):
#   glos/glos-gpu          runs a game on ARM's driver (start.sh calls it)
#   glos/glos-gpu-switch   the root part: driver switch, framebuffer, reset
#   glos/mali.ko           ARM's kernel driver (build-module.sh -> out/mali.ko)
#   glos/glos_fbfix.ko     framebuffer fix module (fbfix/)
#   glos/lib/              ARM's libMali.so and the libEGL/libGLESv2 stubs
#   glos/testing/fbinfo    probe: what fb0 reports (not shipped by the ports)
# Needs the glos-kernel:ready Docker image (kernel/).
set -eu
cd "$(dirname "$0")"
helper/build.sh > /dev/null
fbfix/build.sh > /dev/null
mkdir -p glos/lib glos/testing
cp helper/glos-gpu helper/glos-gpu-switch fbfix/out/glos_fbfix.ko glos/
[ -f out/mali.ko ] && cp out/mali.ko glos/mali.ko
cp stubs/libMali.so stubs/libEGL.so.1 stubs/libGLESv2.so.2 glos/lib/
cp fbfix/out/fbinfo glos/testing/fbinfo
chmod 755 glos/glos-gpu glos/glos-gpu-switch glos/testing/fbinfo
du -sh glos | cut -f1; ls glos glos/lib glos/testing
