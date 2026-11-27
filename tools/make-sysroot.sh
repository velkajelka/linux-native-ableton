#!/bin/sh
# Unpacks the Qt 5 and Boost packages the build needs into .sysroot/ (or $1),
# without installing them: shim/build.sh and tools/make-assets.cpp compile
# against Qt 5 headers, and the bundled innoextract against Boost. Arch-based
# systems (pacman); elsewhere, install your distribution's Qt 5 and Boost
# development packages and point SYSROOT at /.
set -eu
DEST=$(realpath -m "${1:-$(dirname "$0")/../.sysroot}")
mkdir -p "$DEST/pkgs"
cd "$DEST/pkgs"
# pacman gives each package's location, SHA-256 and signature from its own
# database. A package is used only when the signature (checked with pacman's
# keyring) and the digest both match, cached or not, so a validly signed
# archive of another version is refused too. A download lands under its final
# name only after both checks. A package without a signature in the database
# takes the mirror's detached .sig instead.
ok() { [ -s "$1" ] && [ -s "$3" ] && pacman-key --verify "$3" "$1" >/dev/null 2>&1 \
    && [ "$(sha256sum < "$1" | cut -d' ' -f1)" = "$2" ]; }
pacman -Sp --noconfirm --print-format '%l %h %g' qt5-base boost boost-libs | grep -E '^(https?|file)://' | grep -v translations |
    while read -r url sum sig; do
        f=${url##*/}
        if [ -n "$sig" ]; then
            printf '%s' "$sig" | base64 -d > "$f.sig.db" || { echo "error: bad signature data for $f" >&2; exit 1; }
        else
            rm -f "$f.sig.db"
            curl -fsSL -o "$f.sig.db" "$url.sig" || { echo "error: no signature for $f" >&2; exit 1; }
        fi
        if ! ok "$f" "$sum" "$f.sig.db"; then
            rm -f "$f"
            curl -fsSL -o "$f.part" "$url" || { echo "error: cannot download $url" >&2; exit 1; }
            ok "$f.part" "$sum" "$f.sig.db" || { rm -f "$f.part"; echo "error: signature or digest does not match: $f" >&2; exit 1; }
            mv "$f.part" "$f"
        fi
        bsdtar -xf "$f" -C "$DEST" usr
    done
echo "sysroot ready: $DEST"
