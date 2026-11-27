#!/bin/sh
# The text-drawing libraries a plug-in loads (FreeType, HarfBuzz, libpng, Cairo,
# Pango, Fontconfig) bind nothing into Live, whatever its name, except the
# zlib, expat, libc and C++ runtime functions Live also exports (left as they
# are). Unlike
# font-bindings.sh this test does not pick names by prefix: every binding from
# those libraries into Live counts.
# Usage: tests/live-bindings.sh [install folder]   (default ~/.local/share/ableton-live-12)
set -eu
PREFIX=${1:-$HOME/.local/share/ableton-live-12}
APP=$PREFIX/push3_opt/push3
LIVE=$APP/Live
[ -x "$LIVE" ] || { echo "FAIL: no Live at $LIVE"; exit 2; }
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
pre=""
for l in libpangocairo-1.0.so.0 libpangoft2-1.0.so.0; do
    p=$(ldconfig -p | awk -v n="$l" '$1 == n && /x86-64/ {print $NF; exit}')
    [ -n "$p" ] || { echo "FAIL: $l not installed"; exit 2; }
    pre="$pre${pre:+:}$p"
done
audit=${LIVE_AUDIT-$PREFIX/shim/libhideexports.so}
[ -e "$audit" ] || audit=""
env -u DISPLAY -u WAYLAND_DISPLAY \
    LD_LIBRARY_PATH="$APP/lib:$PREFIX/compat_lib" LD_PRELOAD="$pre" ${audit:+LD_AUDIT="$audit"} \
    LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=1 LD_DEBUG=bindings LD_DEBUG_OUTPUT="$out/b" \
    "$LIVE" > "$out/trace" 2>&1 || true
cat "$out"/b.* > "$out/all" 2>/dev/null || { echo "FAIL: loader wrote no bindings"; exit 2; }
total=$(grep -c ': normal symbol' "$out/all" || true)
[ "$total" -gt 1000 ] || { echo "FAIL: only $total bindings traced"; exit 2; }
# binding file <from> [0] to <to> [0]: normal symbol `name'
grep -E "binding file [^ ]*/lib(freetype|harfbuzz|png16|cairo|pango[^ ]*|fontconfig)\.so[^ ]* \[0\] to [^ ]*/Live \[0\]: normal symbol" \
    "$out/all" | sed -E 's/.*binding file ([^ ]*) .*symbol `([^'"'"']*).*/\1 \2/' | awk '{n=split($1,a,"/"); print a[n], $2}' |
    grep -vE ' (inflate|deflate|crc32|adler32|gz|compress|uncompress|zlib|XML_|dup2$|pthread_|_Z)' | sort -u > "$out/bad" || true
echo "traced $total bindings${audit:+, audit $audit}"
if [ -s "$out/bad" ]; then
    echo "FAIL: $(wc -l < "$out/bad") bindings from text-drawing libraries go into Live:"
    sed 's/^/  /' "$out/bad" | head -20
    exit 1
fi
echo "PASS: no text-drawing library binds into Live apart from the out-of-scope runtime functions"
