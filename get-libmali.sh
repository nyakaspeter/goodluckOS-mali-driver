#!/bin/sh
# ARM's Mali-400 user-space driver isn't in this repo (it is ARM's,
# proprietary): give this script the r6p2-01rel0 fbdev armhf libMali.so
# (Bootlin publishes ARM's r6p2 builds, with ARM's licence, at
# github.com/bootlin/mali-blobs). It checks the file is that build, keeps it as
# stubs/libMali.so.orig and writes stubs/libMali.so with the PT_GNU_STACK
# flags' execute bit cleared (glibc 2.41 and newer refuse to dlopen a library
# that asks for an executable stack). assemble-glos.sh then puts it in glos/lib/.
#   ./get-libmali.sh path/to/libMali.so
set -eu
cd "$(dirname "$0")"
src=${1:?usage: ./get-libmali.sh path/to/libMali.so (r6p2-01rel0, fbdev, armhf)}
want_orig=6eaec1a71a2bf2beab30b9e443bf7acf
want_patched=57ad4c663aa553f052854893c685450d
md5() { if command -v md5sum >/dev/null; then md5sum "$1" | cut -d' ' -f1; else md5 -q "$1"; fi; }
got=$(md5 "$src")
[ "$got" = "$want_orig" ] || { echo "get-libmali.sh: $src is not the r6p2-01rel0 fbdev build (md5 $got, want $want_orig)" >&2; exit 1; }
cp "$src" stubs/libMali.so.orig
python3 - stubs/libMali.so.orig stubs/libMali.so <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
phoff, = struct.unpack_from('<I', d, 0x1c)
phentsize, phnum = struct.unpack_from('<HH', d, 0x2a)
for i in range(phnum):
    o = phoff + i * phentsize
    if struct.unpack_from('<I', d, o)[0] == 0x6474e551:    # PT_GNU_STACK
        flags, = struct.unpack_from('<I', d, o + 24)
        struct.pack_into('<I', d, o + 24, flags & ~1)      # no PF_X
open(sys.argv[2], 'wb').write(d)
PY
chmod 755 stubs/libMali.so
got=$(md5 stubs/libMali.so)
[ "$got" = "$want_patched" ] || { echo "get-libmali.sh: patched file md5 $got, want $want_patched" >&2; exit 1; }
echo "stubs/libMali.so ready (md5 $got)"
