#!/bin/sh
# Builds the Browser app: cyapp/Browser.app (kept in the repository) and
# src/cy_browser_app.h (the same, for the firmware to put on the Cybiko).
# Needs the GNU H8 tools (Debian/Ubuntu: binutils-h8300-hms gcc-h8300-hms)
# and Python 3.
set -e
cd "$(dirname "$0")/.."
out=build/cyapp
mkdir -p $out
CFLAGS="-ms -Os -fomit-frame-pointer -Wall -Wextra -ffreestanding -fno-builtin"
objs=""
for f in cyapp/*.c; do
    o=$out/$(basename "$f" .c).o
    h8300-hitachi-coff-gcc $CFLAGS -c "$f" -o "$o"
    objs="$objs $o"
done
h8300-hitachi-coff-as cyapp/rt.s -o $out/rt.o
for base in 0 0x10000; do
    h8300-hitachi-coff-ld -mh8300s -T cyapp/link.ld -Ttext $base -o $out/at$base.coff $objs $out/rt.o
done
python3 tools/cybiko_mkapp.py --at0 $out/at0.coff --at1 $out/at0x10000.coff \
    --name Browser --icon cyapp/icon.txt --help-file cyapp/help.txt \
    --header src/cy_browser_app.h --symbol cy_browser_app cyapp/Browser.app
