#!/bin/sh
# Device sizes of real widgets at a fractional scale (shim/chrome.cpp): builds
# tests/chrome-scale.cpp with the shim's chrome.cpp against the Qt 5 in
# .sysroot/ (tools/make-sysroot.sh) and runs it on Qt's offscreen platform at
# QT_SCALE_FACTOR=1.325, comparing with the windows' backing stores. No display
# is needed.
# Usage: tests/chrome-scale.sh [shim folder]   (default: the repo's shim/)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
SYSROOT=${SYSROOT:-$REPO/.sysroot}
[ -d "$SYSROOT/usr/include/qt" ] || { echo "FAIL: no Qt 5 in $SYSROOT (run tools/make-sysroot.sh)"; exit 2; }
[ -f "$SHIM/chrome.cpp" ] || { echo "FAIL: no chrome.cpp in $SHIM"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cflags=$(PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --cflags Qt5Widgets)
qtver=$(PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --modversion Qt5Gui)
# shellcheck disable=SC2086
if ! g++ -O1 -std=c++17 -fPIC $cflags -I"$SYSROOT/usr/include/qt/QtGui/$qtver" -I"$SYSROOT/usr/include/qt/QtGui/$qtver/QtGui" \
        -I"$SHIM" "$REPO/tests/chrome-scale.cpp" "$SHIM/chrome.cpp" -o "$T/chrome-scale" \
        -L"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/usr/lib" -lQt5Widgets -lQt5Gui -lQt5Core > "$T/build.log" 2>&1; then
    echo "FAIL: test program did not build"; tail -15 "$T/build.log"; exit 1
fi
mkdir -p "$T/run"
env -u DISPLAY -u WAYLAND_DISPLAY -u QT_SCREEN_SCALE_FACTORS -u QT_AUTO_SCREEN_SCALE_FACTOR QT_SCALE_FACTOR=1.325 \
    LD_LIBRARY_PATH="$SYSROOT/usr/lib" QT_PLUGIN_PATH="$SYSROOT/usr/lib/qt/plugins" \
    QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="$T/run" "$T/chrome-scale"
