#!/bin/sh
# Builds the Cybiko programs here into CyOS applications and the C arrays the
# firmware and PC tools install from. Needs the GNU H8 tools
# (Debian/Ubuntu: binutils-h8300-hms gcc-h8300-hms). The generated files are
# committed, so a firmware build does not need them. APP_OUT=DIR also
# copies the .app files there (launcher/Browser.app is kept in the repo).
set -e
cd "$(dirname "$0")"
tools=../tools
out=../src
tmp=$(mktemp -d)
CFLAGS="-ms -Os -fomit-frame-pointer -Wall -Wextra -ffreestanding"

app() {    # app NAME SYMBOL DIR
    for f in "$3"/*.c; do
        h8300-hitachi-coff-gcc $CFLAGS -c "$f" -o "$tmp/$(basename "$f" .c).o"
    done
    h8300-hitachi-coff-as cyapp.s -o "$tmp/cyapp.o"
    for base in 0 0x10000; do
        h8300-hitachi-coff-ld -mh8300s -T link.ld -Ttext $base -o "$tmp/at$base.coff" \
            "$tmp"/*.o
    done
    python3 $tools/cybiko_mkapp.py --at0 "$tmp/at0.coff" --at1 "$tmp/at0x10000.coff" \
        --name "$1" --icon "$3/icon.txt" --help-file "$3/help.txt" \
        --header "$out/$2.h" --symbol "$2" "$tmp/$1.app"
    rm -f "$tmp"/*.o "$tmp"/*.coff
}

app Browser cy_browser_app browser
[ -n "$APP_OUT" ] && cp "$tmp"/*.app "$APP_OUT"/
rm -rf "$tmp"
