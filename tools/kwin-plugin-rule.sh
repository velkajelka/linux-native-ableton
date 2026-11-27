#!/bin/sh
# Removes the KWin window rule earlier versions added for Live's plug-in editor
# windows ([ableton-live-plugin-editors] in kwinrulesrc, "Minimized: Force No").
# That rule took the minimize button off editors, but it also stopped KWin
# minimizing them with Live's main window, so editors stayed on screen when
# Live was minimized. The shim runs this at every start; the uninstaller runs
# it with --remove (same effect). Every other rule is left as it is, and the
# file is not written at all when the rule is not there.
# Runs only in a KDE session (Plasma 5 or 6).
set -eu
case ":${XDG_CURRENT_DESKTOP:-}:" in *:KDE:*) ;; *) exit 0 ;; esac
if command -v kwriteconfig6 >/dev/null 2>&1 && command -v kreadconfig6 >/dev/null 2>&1; then
    v=6
elif command -v kwriteconfig5 >/dev/null 2>&1 && command -v kreadconfig5 >/dev/null 2>&1; then
    v=5
else
    exit 0
fi
RC=kwinrulesrc
ID=ableton-live-plugin-editors
FILE=$(readlink -f "${XDG_CONFIG_HOME:-$HOME/.config}/$RC") # a symlinked file stays a symlink
read_key() { "kreadconfig$v" --file "$RC" --group "$1" --key "$2"; }
write() { "kwriteconfig$v" --file "$RC" "$@"; }

# The rule list: [General] Order on KWin 6; [General] rules plus count on
# KWin 5 (KWin 6 still reads those). Both are cleaned.
listed=
for key in Order rules; do
    case ",$(read_key General "$key")," in *",$ID,"*) listed="$listed $key" ;; esac
done
grouped=
! grep -qxF "[$ID]" "$FILE" 2>/dev/null || grouped=1
[ -n "$listed$grouped" ] || exit 0

for key in $listed; do
    list=$(read_key General "$key" | tr ',' '\n' | grep -vx "$ID" | paste -sd, -)
    write --group General --key "$key" "$list"
    if [ "$key" = rules ]; then
        write --group General --key count "$(printf '%s' "$list" | tr ',' '\n' | grep -c .)" || true
    fi
done
# The whole group goes, settings added in System Settings included: every
# line from its header up to the next group's header.
if [ -n "$grouped" ]; then
    # A temporary file of its own, so two helpers starting together never share one.
    tmp=$(mktemp "$FILE.XXXXXX")
    if awk -v head="[$ID]" '/^\[/ { skip = ($0 == head) } !skip' "$FILE" > "$tmp"; then mv "$tmp" "$FILE"; else rm -f "$tmp"; fi
fi
dbus-send --session --type=method_call --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1 || true
