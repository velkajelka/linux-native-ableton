#!/bin/sh
# Live stays out of swap when Plasma starts it (tools/launcher.c): Plasma runs
# each app in a systemd user unit, Live's named app-ableton\x2dlive\x2d12@<id>.service.
# The launcher is built from tools/launcher.c into a fixture install whose Live
# prints its own unit's memory.swap.max, and run in throwaway transient user
# units named like Plasma's and otherwise. No display: an error dialog cannot
# appear. Without a user systemd the checks cannot run: SKIP, said so.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
command -v systemd-run > /dev/null && systemctl --user show-environment > /dev/null 2>&1 ||
    { echo "SKIP: no user systemd here, the swap limit is not checked"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
ROOT=$T/install
mkdir -p "$ROOT/shim" "$ROOT/push3_opt/push3"
gcc -O2 -static -DLIVE_MAJOR='"12"' "$HERE/../tools/launcher.c" -o "$ROOT/push3_opt/push3/AbletonLive12" 2>/dev/null
: > "$ROOT/shim/libmenufix.so"
# Static, so the loader does not act on the fixture's LD_PRELOAD and LD_AUDIT.
cat > "$T/live.c" <<'EOF'
#include <stdio.h>
#include <string.h>
int main(void)
{
    char line[4096], path[4200], value[64] = "<none>";
    FILE *f = fopen("/proc/self/cgroup", "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "0::", 3)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.swap.max", line + 3);
            FILE *s = fopen(path, "r");
            if (s && fgets(value, sizeof value, s))
                value[strcspn(value, "\n")] = 0;
            if (s)
                fclose(s);
        }
    printf("swap.max=%s\n", value);
    return 0;
}
EOF
gcc -O2 -static "$T/live.c" -o "$ROOT/push3_opt/push3/Live"
fail=0
check() { # <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  ok   $1: $3"; else echo "FAIL: $1: expected '$2', got '$3'"; fail=1; fi
}
# run <unit name> [env...]: the launcher in a throwaway transient unit; prints what the fixture Live printed.
run() {
    unit=$1; shift
    systemd-run --user --quiet --wait --pipe --collect --unit="$unit" \
        env -u DISPLAY -u WAYLAND_DISPLAY -u LD_AUDIT LIVE_AVX512EMU=0 "$@" "$ROOT/push3_opt/push3/AbletonLive12" 2>"$T/err" | tail -1
}
check "started by Plasma (Live 12's unit)" "swap.max=0" "$(run "app-ableton\\x2dlive\\x2d12@swaptest$$.service")"
check "started another way" "swap.max=max" "$(run "swaptest-other-$$.service")"
mkdir -p "$T/bin"; ln -s "$(command -v env)" "$T/bin/env"
check "no systemctl on PATH: Live still starts" "swap.max=max" "$(run "app-ableton\\x2dlive\\x2d12@swapnoctl$$.service" PATH="$T/bin")"
[ "$fail" = 0 ] || { sed 's/^/    /' "$T/err" | tail -5; exit 1; }
echo "PASS: Live's own unit stays out of swap, other units are left alone"
