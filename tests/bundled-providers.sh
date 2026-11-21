#!/bin/sh
# Every bundled program and library that uses a FreeType, HarfBuzz or libpng
# name finds it in its own dependencies, so none of them relies on the copies
# built into Live (which libhideexports.so keeps to Live).
# Usage: tests/bundled-providers.sh [install folder]   (default ~/.local/share/ableton-live-12)
set -eu
PREFIX=${1:-$HOME/.local/share/ableton-live-12}
[ -d "$PREFIX" ] || { echo "FAIL: no install at $PREFIX"; exit 2; }
LP="$PREFIX/push3_opt/push3/lib:$PREFIX/compat_lib"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
elves=0; users=0; bad=0
find "$PREFIX" -type f -size +1k -print0 > "$T/files"
while IFS= read -r -d '' f; do
    [ "$(head -c4 "$f" 2>/dev/null | od -An -c | tr -d ' ')" = '177ELF' ] || continue
    case "$f" in */push3_opt/push3/Live) continue ;; esac
    elves=$((elves + 1))
    nm -D --undefined-only "$f" 2>/dev/null | awk '{print $2}' | sed 's/@.*//' | grep -E '^(FT_|hb_|png_)' | sort -u > "$T/und" || true
    [ -s "$T/und" ] || continue
    users=$((users + 1))
    LD_LIBRARY_PATH="$LP" ldd "$f" 2>/dev/null | awk '/=> \//{print $3}' > "$T/deps"
    : > "$T/def"
    while read -r d; do nm -D --defined-only "$d" 2>/dev/null | awk '{print $3}' | sed 's/@.*//' >> "$T/def"; done < "$T/deps"
    sort -u -o "$T/def" "$T/def"
    miss=$(comm -23 "$T/und" "$T/def" | wc -l)
    if [ "$miss" -gt 0 ]; then
        bad=$((bad + 1)); echo "FAIL: ${f#"$PREFIX"/} has $miss names no dependency provides:"
        comm -23 "$T/und" "$T/def" | head -5 | sed 's/^/    /'
    else
        echo "  ok   ${f#"$PREFIX"/}: $(wc -l < "$T/und") names, all provided"
    fi
done < "$T/files"
[ "$elves" -gt 0 ] || { echo "FAIL: no ELF files found under $PREFIX"; exit 2; }
echo "checked $elves ELF files, $users use these names"
[ "$bad" = 0 ] || exit 1
echo "PASS: every user of a FreeType, HarfBuzz or libpng name has its own provider"
