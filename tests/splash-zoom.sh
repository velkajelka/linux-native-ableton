#!/bin/sh
# The splash screen's recorded Zoom (shim/splashzoom.cpp, shim/window.cpp):
# builds tests/splash-zoom.cpp with the shim's splashzoom.cpp (plain C++, no
# Qt) and runs it in a temporary folder, then checks that the installed Live
# still holds the code where the shim's hook expects it. No display is needed.
# Usage: tests/splash-zoom.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
[ -f "$SHIM/splashzoom.cpp" ] || { echo "FAIL: no splashzoom.cpp in $SHIM"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if ! g++ -O1 -std=c++17 -I"$SHIM" "$REPO/tests/splash-zoom.cpp" "$SHIM/splashzoom.cpp" -o "$T/splash-zoom" \
        > "$T/build.log" 2>&1; then
    echo "FAIL: test program did not build"; tail -15 "$T/build.log"; exit 1
fi
mkdir "$T/run"
"$T/splash-zoom" "$T/run"

# Must match kCreateWindow, kCreateWindowPrologue and kSplashCreateReturn in
# shim/window.cpp. Code addresses equal file offsets in this build.
LIVE=$PREFIX/push3_opt/push3/Live
if [ ! -f "$LIVE" ]; then
    echo "PASS (no installed Live at $LIVE: hook site check skipped)"
    exit 0
fi
bytes() { od -A n -t x1 -j "$1" -N "$2" "$LIVE" | tr -d ' \n'; }
create=$((0x20e4770)) ret=$((0x21586e9))
prologue=$(bytes $create 14)
if [ "$prologue" != "554157415641554154534883ec28" ]; then
    echo "FAIL: hooked code is $prologue, not what the shim expects"; exit 1
fi
# A call is e8 and a little-endian signed 32-bit offset from the return site.
call=$(bytes $((ret - 5)) 5)
op=${call%????????}
rel=$(printf '%s' "${call#??}" | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
rel=$((0x$rel))
[ "$rel" -lt $((0x80000000)) ] || rel=$((rel - 0x100000000))
target=$((ret + rel))
if [ "$op" != e8 ] || [ "$target" -ne "$create" ]; then
    echo "FAIL: code before $(printf '%#x' $ret) is $call, not what the shim expects"; exit 1
fi
echo "PASS: recorded Zoom helpers, and the hooked code is where the shim expects it"
