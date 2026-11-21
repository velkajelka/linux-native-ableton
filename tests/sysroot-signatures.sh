#!/bin/sh
# tools/make-sysroot.sh uses a package only when its signature (pacman's
# keyring) and its SHA-256, both from pacman's database, match: a package with
# a bad signature, or a validly signed package of another version, stops the
# script before anything is unpacked; a truncated file in the cache is fetched
# again and the script then succeeds. Offline and independent of the host's
# current package versions: pacman is replaced by a fixture that answers for
# two genuinely signed packages from the local pacman cache, served from a
# temporary folder by file:// URL. Needs pacman (Arch-based systems).
# Usage: tests/sysroot-signatures.sh [make-sysroot.sh]
set -eu
SCRIPT=$(realpath "${1:-$(dirname "$0")/../tools/make-sysroot.sh}")
command -v pacman-key >/dev/null || { echo "SKIP: no pacman"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
# Two small cached packages that have usr/ contents and a signature that verifies.
picked=0
for f in $(ls -S -r /var/cache/pacman/pkg/*.pkg.tar.zst 2>/dev/null | head -200); do
    [ -s "$f.sig" ] && bsdtar -tf "$f" 2>/dev/null | grep -q '^usr/.' && pacman-key --verify "$f.sig" "$f" >/dev/null 2>&1 || continue
    picked=$((picked + 1)); n=$(basename "$f")
    mkdir -p "$T/srv$picked"; cp "$f" "$T/srv$picked/$n"
    eval "P$picked=\$n; S$picked=\$(sha256sum < \"\$f\" | cut -d' ' -f1); G$picked=\$(base64 -w0 < \"\$f.sig\")"
    [ "$picked" = 2 ] && break
done
[ "$picked" = 2 ] || { echo "SKIP: fewer than two signed packages in /var/cache/pacman/pkg"; exit 0; }
mkdir -p "$T/bin"
# pacman -Sp [--print-format FMT] pkgs...: one line for the package the case
# names in $T/answer ("<served file> <digest> <signature>"), formatted as asked.
cat > "$T/bin/pacman" <<EOF
#!/bin/sh
fmt='%l'
while [ \$# -gt 0 ]; do [ "\$1" = --print-format ] && fmt=\$2; shift; done
read -r file sum sig < "$T/answer"
printf '%s\n' "\$fmt" | sed -e "s|%l|file://\$file|" -e "s|%h|\$sum|" -e "s|%g|\$sig|"
EOF
chmod +x "$T/bin/pacman"
fail=0
check() { # <label> <condition result: 0 ok>
    if [ "$2" = 0 ]; then echo "  ok   $1"; else echo "FAIL: $1"; fail=1; fi
}
run() { PATH="$T/bin:$PATH" sh "$SCRIPT" "$T/$1" > "$T/$1.out" 2>&1 && echo 0 || echo 1; }
# 1. The genuine package: unpacked, cached as served.
echo "$T/srv1/$P1 $S1 $G1" > "$T/answer"
st=$(run a)
[ "$st" = 0 ] && [ -d "$T/a/usr" ] && [ "$(sha256sum < "$T/a/pkgs/$P1" | cut -d' ' -f1)" = "$S1" ]
check "genuine package: unpacked" $?
# 2. A signature that does not belong to the package.
echo "$T/srv1/$P1 $S1 $G2" > "$T/answer"
st=$(run b)
[ "$st" != 0 ] && [ ! -d "$T/b/usr" ]
check "wrong signature: stopped, nothing unpacked" $?
# 3. A validly signed package of another version at the expected location.
cp "$T/srv2/$P2" "$T/srv2/$P1"
echo "$T/srv2/$P1 $S1 $G2" > "$T/answer"
st=$(run c)
[ "$st" != 0 ] && [ ! -d "$T/c/usr" ]
check "other signed package, wrong digest: stopped, nothing unpacked" $?
# 4. A truncated file in the cache: fetched again, then unpacked.
mkdir -p "$T/d/pkgs"; head -c 100 "$T/srv1/$P1" > "$T/d/pkgs/$P1"
echo "$T/srv1/$P1 $S1 $G1" > "$T/answer"
st=$(run d)
[ "$st" = 0 ] && [ -d "$T/d/usr" ] && [ "$(sha256sum < "$T/d/pkgs/$P1" | cut -d' ' -f1)" = "$S1" ]
check "truncated cache entry: fetched again and unpacked" $?
[ "$fail" = 0 ] || { for c in a b c d; do sed "s/^/    $c: /" "$T/$c.out" | tail -2; done; exit 1; }
echo "PASS: make-sysroot.sh uses only packages whose signature and digest match"
