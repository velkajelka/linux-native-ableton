#!/bin/sh
# Plug-in editors minimise with Live on KDE: the helper the shim
# runs at every start (tools/kwin-plugin-rule.sh, no arguments) adds no KWin
# rule, and removes the "no minimize" rule earlier versions added, leaving
# every other rule, their order and their settings as they were. Runs against
# stand-in kwinrulesrc files in a temporary XDG_CONFIG_HOME, with no session
# bus, so the desktop's own rules and KWin are never touched.
# Usage: tests/kwin-rule.sh [tools folder]   (default: this repo's tools/)
set -eu
TOOLS=$(cd "${1:-$(dirname "$0")/../tools}" && pwd)
SCRIPT=$TOOLS/kwin-plugin-rule.sh
command -v kwriteconfig6 >/dev/null 2>&1 && command -v kreadconfig6 >/dev/null 2>&1 \
    || { echo "SKIP: no kwriteconfig6/kreadconfig6"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "FAIL: $1"; fail=1; }
helper() { # <config folder> [args]: run the helper as the shim does
    cfg=$1; shift
    env -u DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP=KDE XDG_CONFIG_HOME="$cfg" HOME="$T/home" \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent "$SCRIPT" "$@" >/dev/null 2>&1
}
mkdir -p "$T/home"

# The two other rules, written as KWin writes them; reused in every case.
others() {
    cat <<'EOF'
[rule-a]
Description=Rule A
above=true
aboverule=2
wmclass=^example-a\\s
wmclasscomplete=true
wmclassmatch=3

[rule-b]
Description=Rule B
fsplevel=0
fsplevelrule=2
wmclass=example.b
wmclassmatch=1
EOF
}
ours() {
    cat <<'EOF'
[ableton-live-plugin-editors]
Description=Ableton Live plug-in editors: no minimize
minimize=false
minimizerule=2
wmclass=^ableton-live-plugin\\s
wmclasscomplete=true
wmclassmatch=3

EOF
}

# 1. A desktop without the rule: the helper adds nothing, the file stays byte for byte.
mkdir -p "$T/clean"
{ printf '[General]\nOrder=rule-a,rule-b\n\n'; others; } > "$T/clean/kwinrulesrc"
cp "$T/clean/kwinrulesrc" "$T/clean.before"
helper "$T/clean"
if cmp -s "$T/clean.before" "$T/clean/kwinrulesrc"; then ok "no rule present: kwinrulesrc is left byte for byte"
else bad "no rule present: the helper changed kwinrulesrc:"; diff "$T/clean.before" "$T/clean/kwinrulesrc" | sed 's/^/      /'; fi

# 2. An install that has the rule (KWin 6 Order list, ours in the middle).
mkdir -p "$T/old"
{ printf '[General]\nOrder=rule-a,ableton-live-plugin-editors,rule-b\n\n'; ours; others; } > "$T/old/kwinrulesrc"
helper "$T/old"
order=$(kreadconfig6 --file "$T/old/kwinrulesrc" --group General --key Order)
[ "$order" = "rule-a,rule-b" ] && ok "rule present: Order is rule-a,rule-b" || bad "rule present: Order is '$order', expected 'rule-a,rule-b'"
if grep -q 'ableton-live-plugin' "$T/old/kwinrulesrc"; then bad "rule present: the rule is still in kwinrulesrc"
else ok "rule present: the rule's group is gone"; fi
others > "$T/others.expected"
sed -n '/^\[rule-a\]/,$p' "$T/old/kwinrulesrc" > "$T/others.got"
if cmp -s "$T/others.expected" "$T/others.got"; then ok "rule present: the other rules are unchanged"
else bad "rule present: the other rules changed:"; diff "$T/others.expected" "$T/others.got" | sed 's/^/      /'; fi

# 3. The KWin 5 form (rules plus count).
mkdir -p "$T/kwin5"
{ printf '[General]\ncount=3\nrules=rule-a,rule-b,ableton-live-plugin-editors\n\n'; ours; others; } > "$T/kwin5/kwinrulesrc"
helper "$T/kwin5"
rules=$(kreadconfig6 --file "$T/kwin5/kwinrulesrc" --group General --key rules)
count=$(kreadconfig6 --file "$T/kwin5/kwinrulesrc" --group General --key count)
[ "$rules" = "rule-a,rule-b" ] && [ "$count" = 2 ] && ok "KWin 5 list: rules=rule-a,rule-b, count=2" \
    || bad "KWin 5 list: rules='$rules' count='$count', expected rule-a,rule-b and 2"
grep -q 'ableton-live-plugin' "$T/kwin5/kwinrulesrc" && bad "KWin 5 list: the rule is still in kwinrulesrc" \
    || ok "KWin 5 list: the rule's group is gone"

# 4. A second start after the removal leaves the file as the first left it.
cp "$T/old/kwinrulesrc" "$T/old.after1"
helper "$T/old"
cmp -s "$T/old.after1" "$T/old/kwinrulesrc" && ok "second start: kwinrulesrc is left byte for byte" \
    || bad "second start: the helper changed kwinrulesrc again"

# 5. The uninstaller's call (--remove) still works.
mkdir -p "$T/uninst"
{ printf '[General]\nOrder=ableton-live-plugin-editors,rule-a,rule-b\n\n'; ours; others; } > "$T/uninst/kwinrulesrc"
helper "$T/uninst" --remove
order=$(kreadconfig6 --file "$T/uninst/kwinrulesrc" --group General --key Order)
[ "$order" = "rule-a,rule-b" ] && ! grep -q 'ableton-live-plugin' "$T/uninst/kwinrulesrc" \
    && ok "--remove: the rule is removed, Order rule-a,rule-b" || bad "--remove: Order '$order' or the rule is still there"

# 6. KWin 5 list: the other rules keep their settings too.
sed -n '/^\[rule-a\]/,$p' "$T/kwin5/kwinrulesrc" > "$T/others5.got"
if cmp -s "$T/others.expected" "$T/others5.got"; then ok "KWin 5 list: the other rules are unchanged"
else bad "KWin 5 list: the other rules changed:"; diff "$T/others.expected" "$T/others5.got" | sed 's/^/      /'; fi

# 7. A rule the user added settings to is removed whole.
mkdir -p "$T/custom"
{ printf '[General]\nOrder=rule-a,ableton-live-plugin-editors,rule-b\n\n'; ours | sed 's/^minimizerule=2$/minimizerule=2\nabove=true\naboverule=2/'; others; } > "$T/custom/kwinrulesrc"
helper "$T/custom"
# Everything before [rule-a] must be exactly the [General] list: no stray key of the old group.
if grep -q 'ableton-live-plugin' "$T/custom/kwinrulesrc" \
    || [ "$(sed '/^\[rule-a\]/,$d' "$T/custom/kwinrulesrc")" != "$(printf '[General]\nOrder=rule-a,rule-b\n')" ]; then
    bad "customised rule: part of the rule is still in kwinrulesrc:"; sed '/^\[rule-a\]/,$d' "$T/custom/kwinrulesrc" | sed 's/^/      /'
else ok "customised rule: the whole group is gone"; fi
sed -n '/^\[rule-a\]/,$p' "$T/custom/kwinrulesrc" > "$T/othersc.got"
cmp -s "$T/others.expected" "$T/othersc.got" && ok "customised rule: the other rules are unchanged" \
    || bad "customised rule: the other rules changed"

# 8. A group left without its list entry or its match keys is still removed.
mkdir -p "$T/stray"
{ printf '[General]\nOrder=rule-a,rule-b\n\n[ableton-live-plugin-editors]\nabove=true\naboverule=2\n\n'; others; } > "$T/stray/kwinrulesrc"
helper "$T/stray"
grep -q 'ableton-live-plugin' "$T/stray/kwinrulesrc" && bad "stray group: still in kwinrulesrc" || ok "stray group: removed"
order=$(kreadconfig6 --file "$T/stray/kwinrulesrc" --group General --key Order)
[ "$order" = "rule-a,rule-b" ] && ok "stray group: Order is rule-a,rule-b" || bad "stray group: Order is '$order'"

[ "$fail" = 0 ] || exit 1
echo "PASS: the KWin helper adds no rule and removes the old one, other rules untouched"
