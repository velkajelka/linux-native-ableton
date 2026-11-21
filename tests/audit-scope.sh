#!/bin/sh
# libhideexports.so acts only on .../push3_opt/push3/Live, and only on its
# FT_/hb_/png_ functions; it links nothing and exports only the two audit entry
# points. A fixture program exports decoys of those names (and of other names),
# a library linked to it calls them, and a provider library defines them too:
# each call reports which definition the loader picked.
# Usage: tests/audit-scope.sh [libhideexports.so]   (default: build it from shim/)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
MOD=${1:-}
if [ -z "$MOD" ]; then
    MOD=$T/libhideexports.so
    gcc -O2 -fPIC -shared -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -Wl,--no-undefined \
        "$HERE/../shim/hideexports.c" -o "$MOD"
fi
fail=0
check() { # <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  ok   $1: $3"; else echo "FAIL: $1: expected '$2', got '$3'"; fail=1; fi
}

# No libraries, only la_version and la_objopen.
check "module links no library" "" "$(readelf -d "$MOD" | grep NEEDED || true)"
check "module exports" "la_objopen la_version" "$(nm -D --defined-only "$MOD" | awk '{print $3}' | sort | tr '\n' ' ' | sed 's/ $//')"

cat > "$T/prov.c" <<'EOF'
const char *FT_fixture(void) { return "system"; }
const char *hb_fixture(void) { return "system"; }
const char *png_fixture(void) { return "system"; }
const char *keep_fixture(void) { return "system"; }
const char *hbx_fixture(void) { return "system"; }
int hb_data = 2;
EOF
cat > "$T/cons.c" <<'EOF'
#include <stdio.h>
const char *FT_fixture(void), *hb_fixture(void), *png_fixture(void), *keep_fixture(void), *hbx_fixture(void);
extern int hb_data;
void report(void)
{
    printf("FT_fixture=%s hb_fixture=%s png_fixture=%s keep_fixture=%s hbx_fixture=%s hb_data=%s\n",
           FT_fixture(), hb_fixture(), png_fixture(), keep_fixture(), hbx_fixture(), hb_data == 1 ? "program" : "system");
}
EOF
cat > "$T/main.c" <<'EOF'
const char *FT_fixture(void) { return "program"; }
const char *hb_fixture(void) { return "program"; }
const char *png_fixture(void) { return "program"; }
const char *keep_fixture(void) { return "program"; }
const char *hbx_fixture(void) { return "program"; }
int hb_data = 1;
void report(void);
int main(void) { report(); return 0; }
EOF
gcc -O2 -fPIC -shared "$T/prov.c" -o "$T/libprov.so"
gcc -O2 -fPIC -shared "$T/cons.c" -o "$T/libcons.so" -L"$T" -lprov -Wl,--no-as-needed
gcc -O2 -fPIE -pie -rdynamic "$T/main.c" -o "$T/prog" -L"$T" -lcons -Wl,--no-as-needed -Wl,-rpath-link,"$T"

hidden="FT_fixture=system hb_fixture=system png_fixture=system keep_fixture=program hbx_fixture=program hb_data=program"
visible="FT_fixture=program hb_fixture=program png_fixture=program keep_fixture=program hbx_fixture=program hb_data=program"
for case in "push3_opt/push3/Live:$hidden" "push3_opt/push3/Push3:$visible" "other/Live:$visible" "push3_opt/push3/Live.bak:$visible"; do
    rel=${case%%:*}; want=${case#*:}
    mkdir -p "$T/$(dirname "$rel")"
    cp "$T/prog" "$T/$rel"
    got=$(LD_LIBRARY_PATH="$T" LD_AUDIT="$MOD" "$T/$rel")
    check "$rel" "$want" "$got"
done
# Control: without the module the program at the Live path keeps its names.
check "push3_opt/push3/Live without the module" "$visible" "$(LD_LIBRARY_PATH="$T" "$T/push3_opt/push3/Live")"

[ "$fail" = 0 ] || exit 1
echo "PASS: the module hides FT_/hb_/png_ functions of push3_opt/push3/Live only"
