#!/bin/sh
# The launcher sets LD_AUDIT to shim/libhideexports.so by its absolute path when
# the file is there, leaves it out when the file is missing or
# LIVE_HIDE_EXPORTS=0, and keeps a user's own LD_AUDIT entries after it. The
# launcher is built from tools/launcher.c and run in a fixture install whose
# Live prints the variable. No display: an error dialog cannot appear.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
ROOT=$T/install
mkdir -p "$ROOT/shim" "$ROOT/push3_opt/push3"
gcc -O2 -static -DLIVE_MAJOR='"12"' "$HERE/../tools/launcher.c" -o "$ROOT/push3_opt/push3/AbletonLive12" 2>/dev/null
: > "$ROOT/shim/libmenufix.so"
# Static, so the loader does not act on the fixture's own LD_PRELOAD and LD_AUDIT.
printf '#include <stdio.h>\n#include <stdlib.h>\nint main(void){const char *v=getenv("LD_AUDIT");printf("LD_AUDIT=%%s\\n",v?v:"<unset>");return 0;}\n' > "$T/live.c"
gcc -O2 -static "$T/live.c" -o "$ROOT/push3_opt/push3/Live"
MOD=$ROOT/shim/libhideexports.so
fail=0
# LIVE_AVX512EMU=0: the fixture needs no emulator, also on CPUs without AVX-512.
run() { env -u DISPLAY -u WAYLAND_DISPLAY -u LD_AUDIT -u LIVE_HIDE_EXPORTS LIVE_AVX512EMU=0 "$@" "$ROOT/push3_opt/push3/AbletonLive12" 2>"$T/err"; }
check() { # <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  ok   $1: $3"; else echo "FAIL: $1: expected '$2', got '$3'"; fail=1; fi
}

: > "$MOD"
check "module present" "LD_AUDIT=$MOD" "$(run)"
check "user's own entries kept" "LD_AUDIT=$MOD:/opt/user-audit.so" "$(run LD_AUDIT=/opt/user-audit.so)"
check "LIVE_HIDE_EXPORTS=0" "LD_AUDIT=<unset>" "$(run LIVE_HIDE_EXPORTS=0)"
check "LIVE_HIDE_EXPORTS=1" "LD_AUDIT=$MOD" "$(run LIVE_HIDE_EXPORTS=1)"
rm "$MOD"
check "module missing" "LD_AUDIT=<unset>" "$(run)"

[ "$fail" = 0 ] || exit 1
echo "PASS: the launcher sets LD_AUDIT as expected"
