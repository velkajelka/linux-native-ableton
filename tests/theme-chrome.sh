#!/bin/sh
# Menus, title bar colours and window edges (shim/chrome.cpp): builds
# tests/theme-chrome.cpp with the shim's chrome.cpp against the Qt 5 in
# .sysroot/ (tools/make-sysroot.sh) and runs it on Qt's offscreen platform.
# Checks the menu style (square body and frame, separators, hover plate, row
# and column sizes, dimmer shortcuts, one label column), its colours for two
# themes, loading Live's menu font (read in place from the installed Live), the
# window colour scheme file and the edge fill. No display is needed.
# Usage: tests/theme-chrome.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
SYSROOT=${SYSROOT:-$REPO/.sysroot}
[ -d "$SYSROOT/usr/include/qt" ] || { echo "FAIL: no Qt 5 in $SYSROOT (run tools/make-sysroot.sh)"; exit 2; }
[ -f "$SHIM/chrome.cpp" ] || { echo "FAIL: no chrome.cpp in $SHIM"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cflags=$(PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --cflags Qt5Widgets)
# shellcheck disable=SC2086
if ! g++ -O1 -std=c++17 -fPIC $cflags -I"$SHIM" "$REPO/tests/theme-chrome.cpp" "$SHIM/chrome.cpp" -o "$T/theme-chrome" \
        -L"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/usr/lib" -lQt5Widgets -lQt5Gui -lQt5Core > "$T/build.log" 2>&1; then
    echo "FAIL: test program did not build"; tail -15 "$T/build.log"; exit 1
fi
mkdir -p "$T/run"
env -u DISPLAY -u WAYLAND_DISPLAY LD_LIBRARY_PATH="$SYSROOT/usr/lib" QT_PLUGIN_PATH="$SYSROOT/usr/lib/qt/plugins" \
    QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="$T/run" "$T/theme-chrome" "$T" \
    "$PREFIX/push3_opt/push3/products/push3/assets/fonts/AbletonSansSmall-Regular.otf"
