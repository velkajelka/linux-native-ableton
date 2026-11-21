#!/bin/sh
# Verify: builds the shim libraries, the AVX-512 emulator, both launchers and
# make-assets from source (copies of shim/, avx512emu/ and tools/ in a temp
# folder, with the project's own build scripts; Qt 5 from .sysroot/, see
# tools/make-sysroot.sh), then runs the headless checks with the freshly built
# audit module and shim. The bundled innoextract (tools/bundled/) stays
# prebuilt and is not rebuilt here. Red on any failure.
# Usage: tests/verify.sh [install folder]   (default ~/.local/share/ableton-live-12)
# VERIFY_OUT=<repo> copies the built binaries there after a pass (to commit them).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${1:-$HOME/.local/share/ableton-live-12}
SYSROOT=${SYSROOT:-$REPO/.sysroot}
[ -d "$SYSROOT/usr/include/qt" ] || { echo "FAIL: no Qt 5 in $SYSROOT (run tools/make-sysroot.sh)"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp -r "$REPO/shim" "$REPO/avx512emu" "$REPO/tools" "$T/"
rm -f "$T"/shim/*.so "$T"/shim/*.so.* "$T/tools/AbletonLive11-launcher" "$T/tools/AbletonLive12-launcher" "$T/tools/make-assets"
# pkg-config answers Qt 5 from the sysroot and everything else from the system.
mkdir -p "$T/bin"
cat > "$T/bin/pkg-config" <<WRAP
#!/bin/sh
case "\$*" in
*Qt5*) qt=; other=
       for a in "\$@"; do case \$a in Qt5*) qt="\$qt \$a" ;; --*) ;; *) other="\$other \$a" ;; esac; done
       PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT /usr/bin/pkg-config --cflags \$qt
       [ -z "\$other" ] || /usr/bin/pkg-config --cflags \$other ;;
*) exec /usr/bin/pkg-config "\$@" ;;
esac
WRAP
chmod +x "$T/bin/pkg-config"
log=$T/build.log
step() { printf '%-38s' "$1"; shift; if "$@" >>"$log" 2>&1; then echo ok; else echo FAIL; tail -20 "$log"; exit 1; fi; }
export PATH="$T/bin:$PATH" LIBRARY_PATH="$SYSROOT/usr/lib"
step "build shim (menufix, stubs, audit)" sh "$T/shim/build.sh" "$T/shim/libmenufix.so"
step "build AVX-512 emulator" sh "$T/avx512emu/build.sh" "$T/shim/libavx512emu.so"
step "build launchers" sh "$T/tools/build-launchers.sh"
step "build make-assets" sh -c "cd '$T/tools' && g++ -O2 -std=c++17 \$(pkg-config --cflags Qt5Gui) -o make-assets make-assets.cpp ../shim/glibccompat.cpp -lQt5Gui -lQt5Core -static-libstdc++ -static-libgcc -s"
for f in shim/libmenufix.so shim/libpathfix.so shim/libswupdate.so.0.1 shim/libhideexports.so shim/libavx512emu.so \
         tools/AbletonLive12-launcher tools/AbletonLive11-launcher tools/make-assets; do
    [ -s "$T/$f" ] || { echo "FAIL: $f was not built"; exit 1; }
done
fail=0
for t in "font-bindings.sh $PREFIX" "live-bindings.sh $PREFIX" "audit-scope.sh $T/shim/libhideexports.so" "launcher-env.sh" \
         "helper-env.sh $T/shim/libmenufix.so $PREFIX" "bundled-providers.sh $PREFIX" "sysroot-signatures.sh" \
         "chrome-regressions.sh $T/shim" \
         "chrome-scale.sh $T/shim" \
         "splash-zoom.sh $T/shim $PREFIX" \
         "theme-chrome.sh $T/shim $PREFIX" \
         "vst3-host-context.sh $T/shim $PREFIX" \
         "show-in-folder.sh $T/shim $PREFIX" \
         "kwin-rule.sh $T/tools" \
         "tls-audit-race.sh $T/shim $PREFIX"; do
    name=${t%% *}
    if LIVE_AUDIT=$T/shim/libhideexports.so sh "$REPO/tests/"$t > "$T/$name.out" 2>&1; then
        echo "$name: $(tail -1 "$T/$name.out")"
    else
        echo "$name: FAILED"; sed 's/^/    /' "$T/$name.out" | tail -15; fail=1
    fi
done
[ "$fail" = 0 ] || exit 1
if [ -n "${VERIFY_OUT:-}" ]; then
    for f in shim/libmenufix.so shim/libpathfix.so shim/libswupdate.so.0.1 shim/libhideexports.so shim/libavx512emu.so \
             tools/AbletonLive12-launcher tools/AbletonLive11-launcher tools/make-assets; do
        cp "$T/$f" "$VERIFY_OUT/$f"
    done
    echo "built binaries copied to $VERIFY_OUT"
fi
echo "VERIFY PASS"
