#!/bin/sh
# Plug-ins and the system libraries they load must get the system's FreeType,
# HarfBuzz and libpng, never the copies built into Live. The loader resolves
# every symbol of Live, its libraries and a text-drawing stack (Pango, Cairo)
# in trace mode, without running Live; the test fails if any library other
# than Live itself has a FreeType, HarfBuzz or libpng name bound into Live.
# Usage: tests/font-bindings.sh [install folder]   (default ~/.local/share/ableton-live-12)
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
# The same environment the launcher gives Live, plus the audit module when the
# install has one (LIVE_AUDIT empty: run without it).
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
grep -E "to [^ ]*/Live \[0\]: normal symbol \`(FT_|hb_|png_)" "$out/all" |
    grep -vE "binding file [^ ]*/Live \[0\]" > "$out/bad" || true
bad=$(wc -l < "$out/bad")
pango=$(grep -cE "binding file [^ ]*libpango[^ ]* \[0\] to .*symbol \`hb_" "$out/all" || true)
[ "$pango" -gt 0 ] || { echo "FAIL: Pango's HarfBuzz calls were not traced"; exit 2; }
echo "traced $total bindings ($pango Pango->HarfBuzz)${audit:+, audit $audit}"
fail=0
if [ "$bad" -gt 0 ]; then
    echo "FAIL: $bad FreeType/HarfBuzz/libpng bindings from other libraries go into Live:"
    sed -E 's/.*binding file ([^ ]*) .*symbol `([^'"'"']*).*/\1 \2/' "$out/bad" |
        awk '{n=split($1,a,"/"); c[a[n]]++} END {for (k in c) printf "  %-28s %d\n", k, c[k]}' | sort
    fail=1
fi
undef=$(grep -ci 'undefined symbol' "$out/trace" || true)
if [ "$undef" -gt 0 ]; then
    echo "FAIL: $undef undefined symbols:"; grep -i 'undefined symbol' "$out/trace" | head -5 | sed 's/^/  /'
    fail=1
fi
# Each library that used to be mixed binds the family to the system library
# that defines it: <from library> <name prefix> <expected provider>.
while read -r from fam to; do
    all=$(grep -cE "binding file [^ ]*/$from [^ ]* to [^ ]* [^ ]*: normal symbol \`$fam" "$out/all" || true)
    n=$(grep -cE "binding file [^ ]*/$from [^ ]* to [^ ]*/$to [^ ]*: normal symbol \`$fam" "$out/all" || true)
    if [ "$n" -gt 0 ] && [ "$n" = "$all" ]; then
        echo "  ok   $from $fam* -> $to ($n of $all)"
    else
        echo "FAIL: $from binds $n of $all $fam* names to $to"; fail=1
    fi
done <<'LIST'
libpango-1.0.so.0 hb_ libharfbuzz.so.0
libpangoft2-1.0.so.0 hb_ libharfbuzz.so.0
libharfbuzz.so.0 hb_ libharfbuzz.so.0
libfreetype.so.6 FT_ libfreetype.so.6
libcairo.so.2 FT_ libfreetype.so.6
libfontconfig.so.1 FT_ libfreetype.so.6
libpng16.so.16 png_ libpng16.so.16
LIST
[ "$fail" = 0 ] || exit 1
echo "PASS: no library other than Live binds a FreeType, HarfBuzz or libpng name into Live"
