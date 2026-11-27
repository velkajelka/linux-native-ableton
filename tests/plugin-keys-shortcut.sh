#!/bin/sh
# Keys typed in a plug-in window fire Live's window-wide Qt shortcuts (shim/window.cpp key routing):
# builds tests/plugin-keys-shortcut.cpp with editorcolour.cpp, editorplacement.cpp, pluginclose.cpp and window.cpp against the Qt 5
# in .sysroot/ (tools/make-sysroot.sh) and runs it on its own Xvfb display.
# Usage: tests/plugin-keys-shortcut.sh [shim folder]   (default: the repo's shim/)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
SYSROOT=${SYSROOT:-$REPO/.sysroot}
[ -d "$SYSROOT/usr/include/qt" ] || { echo "FAIL: no Qt 5 in $SYSROOT (run tools/make-sysroot.sh)"; exit 2; }
[ -f "$SHIM/editorcolour.cpp" ] || { echo "FAIL: no editorcolour.cpp in $SHIM"; exit 1; }
command -v Xvfb > /dev/null || { echo "FAIL: Xvfb is needed"; exit 2; }
T=$(mktemp -d)
XPID=
cleanup() { [ -z "$XPID" ] || kill "$XPID" 2> /dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
cflags=$(PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --cflags Qt5Widgets)
# shellcheck disable=SC2086
if ! g++ -O1 -std=c++17 -fPIC $cflags -I"$SHIM" "$REPO/tests/plugin-keys-shortcut.cpp" "$SHIM/editorcolour.cpp" "$SHIM/editorplacement.cpp" "$SHIM/pluginclose.cpp" "$SHIM/window.cpp" "$SHIM/chrome.cpp" \
        "$SHIM/livebuild.cpp" "$SHIM/splashzoom.cpp" -o "$T/plugin-keys-shortcut" \
        -L"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/usr/lib" -lQt5Widgets -lQt5Gui -lQt5Core -lX11 -lXtst -ldl > "$T/build.log" 2>&1; then
    echo "FAIL: test program did not build"; grep -m5 -B2 -A3 "error" "$T/build.log"; exit 1
fi
mkfifo "$T/dpy"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3> "$T/dpy" > "$T/xvfb.log" 2>&1 &
XPID=$!
DPY=$(head -n 1 "$T/dpy")
[ -n "$DPY" ] || { echo "FAIL: Xvfb did not start"; exit 1; }
mkdir -p "$T/run"
status=0
env -u WAYLAND_DISPLAY -u QT_SCALE_FACTOR DISPLAY=":$DPY" LD_LIBRARY_PATH="$SYSROOT/usr/lib" \
    QT_PLUGIN_PATH="$SYSROOT/usr/lib/qt/plugins" QT_QPA_PLATFORM=xcb XDG_RUNTIME_DIR="$T/run" HOME="$T" \
    XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache" \
    "$T/plugin-keys-shortcut" || status=1
exit "$status"
