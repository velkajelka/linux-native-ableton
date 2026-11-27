#!/bin/sh
# Checks for "Show in Folder" (shim/showinfolder.cpp): builds tests/show-in-folder.cpp
# with the shim's showinfolder.cpp, hook.cpp and livebuild.cpp against the
# Qt 5 in .sysroot/ and runs it on private session buses (a socket in a temp
# folder, services read from a temp folder): finding a file manager on the
# bus, and the URIs the file manager receives. Then, when Live 12.4.7 is
# installed, checks that the two functions the shim replaces still hold the
# code the shim expects.
# Usage: tests/show-in-folder.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
SYSROOT=${SYSROOT:-$REPO/.sysroot}
[ -d "$SYSROOT/usr/include/qt" ] || { echo "FAIL: no Qt 5 in $SYSROOT (run tools/make-sysroot.sh)"; exit 2; }
[ -f "$SHIM/showinfolder.cpp" ] || { echo "FAIL: no showinfolder.cpp in $SHIM"; exit 1; }
command -v dbus-run-session >/dev/null || { echo "FAIL: dbus-run-session is needed"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cflags=$(PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --cflags Qt5DBus)
# shellcheck disable=SC2086
if ! g++ -O1 -std=c++17 -fPIC $cflags -I"$SHIM" "$REPO/tests/show-in-folder.cpp" "$SHIM/showinfolder.cpp" "$SHIM/hook.cpp" \
        "$SHIM/livebuild.cpp" -o "$T/show-in-folder" -L"$SYSROOT/usr/lib" -Wl,-rpath-link,"$SYSROOT/usr/lib" \
        -lQt5DBus -lQt5Core > "$T/build.log" 2>&1; then
    echo "FAIL: test program did not build"; tail -15 "$T/build.log"; exit 1
fi

# One bus per case: $1 names the case, $2 the folder of service files.
bus() {
    cat > "$T/$1.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$T/$1.socket</listen>
  <servicedir>$2</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
    env -u DISPLAY -u WAYLAND_DISPLAY LD_LIBRARY_PATH="$SYSROOT/usr/lib" XDG_RUNTIME_DIR="$T/run" \
        dbus-run-session --config-file="$T/$1.conf" -- "$T/show-in-folder" "$1"
}
mkdir -p "$T/run" "$T/empty" "$T/services"
chmod 700 "$T/run"
# A file manager the bus can start, under a file name other than its bus name
# (as Dolphin's org.kde.dolphin.FileManager1.service). Never started here.
printf '[D-BUS Service]\nName=org.freedesktop.FileManager1\nExec=/bin/false\n' > "$T/services/org.example.Stub.service"
status=0
bus none "$T/empty" || status=1
bus activatable "$T/services" || status=1

# Installing: both functions or neither, and a trace line either way
# (tests/show-in-folder-install.cpp).
# shellcheck disable=SC2086
if g++ -O1 -std=c++17 -fPIC $cflags -I"$SHIM" "$REPO/tests/show-in-folder-install.cpp" "$SHIM/showinfolder.cpp" \
        "$SHIM/hook.cpp" "$SHIM/livebuild.cpp" -o "$T/show-in-folder-install" -L"$SYSROOT/usr/lib" \
        -Wl,-rpath-link,"$SYSROOT/usr/lib" -lQt5DBus -lQt5Core > "$T/build-install.log" 2>&1; then
    LD_LIBRARY_PATH="$SYSROOT/usr/lib" "$T/show-in-folder-install" || status=1
else
    echo "FAIL: install test program did not build"; tail -15 "$T/build-install.log"; status=1
fi

# The built shim exports none of this file's functions.
if [ -f "$SHIM/libmenufix.so" ]; then
    exported=$(nm -D --defined-only "$SHIM/libmenufix.so" | awk '{print $3}' \
        | grep -E '^_Z[0-9]*(installShowInFolder|installShowInFolderAt|fileManagerOnBus|sendShowItems)' || true)
    if [ -z "$exported" ]; then
        echo "  ok   libmenufix.so exports none of the Show in Folder functions"
    else
        echo "FAIL: libmenufix.so exports: $(echo $exported | tr '\n' ' ')"; status=1
    fi
else
    echo "  skip no libmenufix.so in $SHIM"
fi

LIVE=$PREFIX/push3_opt/push3/Live
# Only Live 12.4.7 (build ID 0e5b75d8...) has the functions at these addresses.
build=$( (readelf -n "$LIVE" 2>/dev/null || true) | awk '/Build ID/ {print $3; exit}')
case $build in
    0e5b75d8*) ;;
    *) [ -f "$LIVE" ] && echo "  skip Live at $LIVE is not 12.4.7 (build ID $build)"; LIVE=$LIVE.not-12.4.7 ;;
esac
if [ -f "$LIVE" ]; then
    # Live's code segment maps at file offset = address.
    check=$(od -An -tx1 -j $((0x1bf0c50)) -N 12 "$LIVE" | tr -d ' \n')
    show=$(od -An -tx1 -j $((0x1b37850)) -N 17 "$LIVE" | tr -d ' \n')
    if [ "$check" = 4156534883ec58e8c465f3ff ] && [ "$show" = 554157415641554154534881ecb8020000 ]; then
        echo "  ok   Live 12.4.7 holds the code the shim replaces"
    else
        echo "FAIL: Live's code is $check / $show, not what the shim replaces"; status=1
    fi
else
    echo "  skip no Live at $LIVE"
fi
exit $status
