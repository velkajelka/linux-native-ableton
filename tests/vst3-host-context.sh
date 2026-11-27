#!/bin/sh
# Checks for the VST3 factory host context (shim/vst3runloop.cpp): builds
# tests/vst3-host-context.cpp with the shim's vst3runloop.cpp and hook.cpp
# against the Qt 5 in .sysroot/ and runs it with no display. Then, when Live
# 12.4.7 is installed, checks that the place the shim hooks still holds the
# code the shim expects.
# Usage: tests/vst3-host-context.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
SYSROOT=${SYSROOT:-$REPO/.sysroot}
[ -d "$SYSROOT/usr/include/qt" ] || { echo "FAIL: no Qt 5 in $SYSROOT (run tools/make-sysroot.sh)"; exit 2; }
[ -f "$SHIM/vst3runloop.cpp" ] || { echo "FAIL: no vst3runloop.cpp in $SHIM"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cflags=$(PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --cflags Qt5Gui)
# shellcheck disable=SC2086
if ! g++ -O1 -std=c++17 -fPIC $cflags -I"$SHIM" "$REPO/tests/vst3-host-context.cpp" "$SHIM/vst3runloop.cpp" "$SHIM/hook.cpp" \
        -o "$T/vst3-host-context" -L"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/usr/lib" -lQt5Gui -lQt5Core > "$T/build.log" 2>&1; then
    echo "FAIL: test program did not build"; tail -15 "$T/build.log"; exit 1
fi
mkdir -p "$T/run"
status=0
env -u DISPLAY -u WAYLAND_DISPLAY LD_LIBRARY_PATH="$SYSROOT/usr/lib" QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="$T/run" \
    "$T/vst3-host-context" || status=1

LIVE=$PREFIX/push3_opt/push3/Live
# Only Live 12.4.7 (build ID 0e5b75d8...) has the function at that address.
build=$( (readelf -n "$LIVE" 2>/dev/null || true) | awk '/Build ID/ {print $3; exit}')
case $build in
    0e5b75d8*) ;;
    *) [ -f "$LIVE" ] && echo "  skip Live at $LIVE is not 12.4.7 (build ID $build)"; LIVE=$LIVE.not-12.4.7 ;;
esac
if [ -f "$LIVE" ]; then
    # Live's code segment maps at file offset = address.
    code=$(od -An -tx1 -j $((0x5a032d0)) -N 21 "$LIVE" | tr -d ' \n')
    if [ "$code" = 4889f8488b3e4889384885ff7406488b07ff6008c3 ]; then
        echo "  ok   Live 12.4.7 holds the code the shim replaces"
    else
        echo "FAIL: Live's code at 0x5a032d0 is $code, not what the shim replaces"; status=1
    fi
else
    echo "  skip no Live at $LIVE"
fi
exit $status
