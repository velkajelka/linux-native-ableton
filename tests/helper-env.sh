#!/bin/sh
# Programs Live starts never get the launcher's audit module, the first one
# included: the KWin rule helper (tools/kwin-plugin-rule.sh), which the shim
# starts while it sets up Live's environment. A user's own LD_AUDIT entries
# stay. The shim is loaded into a fixture "Live" in a fixture install whose
# helper script records its environment. No display is needed.
# Usage: tests/helper-env.sh <libmenufix.so> [install folder for Live's bundled Qt]
set -eu
SHIM=$(realpath "$1")
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
LIBS=$PREFIX/push3_opt/push3/lib:$PREFIX/compat_lib
[ -d "$PREFIX/push3_opt/push3/lib" ] || { echo "FAIL: no install at $PREFIX"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/root/push3_opt/push3" "$T/root/tools" "$T/home"
printf 'int main(void){return 0;}\n' > "$T/live.c"
gcc -O2 "$T/live.c" -o "$T/root/push3_opt/push3/Live"
cat > "$T/root/tools/kwin-plugin-rule.sh" <<EOF
#!/bin/sh
printf '%s\n' "\${LD_AUDIT-<unset>}" > "$T/seen.tmp" && mv "$T/seen.tmp" "$T/seen"
EOF
chmod +x "$T/root/tools/kwin-plugin-rule.sh"
MOD=$T/root/shim/libhideexports.so
mkdir -p "$T/root/shim"
: > "$MOD"
fail=0
run() { # <LD_AUDIT value> <expected value seen by the helper>
    rm -f "$T/seen"
    env -u DISPLAY -u WAYLAND_DISPLAY -u LIVE_USER_HOME HOME="$T/home" LD_LIBRARY_PATH="$LIBS" \
        LD_PRELOAD="$SHIM" LD_AUDIT="$1" "$T/root/push3_opt/push3/Live" 2>/dev/null || true
    i=0
    while [ ! -e "$T/seen" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    got=$(cat "$T/seen" 2>/dev/null || echo "<helper did not run>")
    if [ "$got" = "$2" ]; then echo "  ok   LD_AUDIT=$1 -> helper sees $got"; else echo "FAIL: LD_AUDIT=$1: helper saw '$got', expected '$2'"; fail=1; fi
}
run "$MOD" "<unset>"
run "$MOD:$T/user-audit.so" "$T/user-audit.so"
[ "$fail" = 0 ] || exit 1
echo "PASS: the KWin rule helper gets no audit module from the launcher"
