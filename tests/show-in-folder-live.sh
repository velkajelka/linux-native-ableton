#!/bin/sh
# "Show in Folder" in Live's browser opens the file manager at the file.
# Starts the installed Live on its own Xvfb display with a throwaway HOME, no network and a private session
# bus whose only file manager is a stub that writes down every file it is
# asked to show. A silent WAV named "Bass C#2, 100% open.wav" sits in the User
# Library; the test right-clicks it in the browser and chooses Show in Folder.
# Checks:
#   show:   the stub is asked for exactly that file, as one URI (red before
#           the fix: the item is greyed out and nothing arrives);
#   repeat: opening the menu ten times and choosing the item three times
#           leaves Live running, with three identical requests.
# Runs Live (about two minutes), so it is on demand, not in tests/verify.sh.
# Never touches the real ~/.config/Ableton: everything lives in a temp folder.
# Usage: tests/show-in-folder-live.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12)
#        SHOW_IN_FOLDER_KEEP=1 keeps the temp folder (screenshots, logs).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
APP=$PREFIX/push3_opt/push3
[ -f "$SHIM/libmenufix.so" ] || { echo "FAIL: no libmenufix.so in $SHIM"; exit 1; }
[ -x "$APP/AbletonLive12" ] || { echo "FAIL: no installed Live at $APP"; exit 1; }
for tool in Xvfb xdotool import dbus-daemon unshare python3; do
    command -v $tool >/dev/null || { echo "FAIL: $tool is needed"; exit 1; }
done
python3 -c 'from gi.repository import Gio' 2>/dev/null || { echo "FAIL: python3 gi (Gio) is needed"; exit 1; }

# Live runs once per user: a second copy hands over to the first and never
# shows a window here.
if pgrep -u "$(id -u)" -f 'push3_opt/push3/(AbletonLive12|Live)( |$)' >/dev/null; then
    echo "FAIL: Live is already running; close it and run this again"; exit 1
fi

T=$(mktemp -d)
XPID= LPID= BPID=
cleanup() {
    if [ -n "$LPID" ]; then # Live and its helpers share the session started by setsid
        kill -TERM -- -"$LPID" 2>/dev/null || true
        i=0
        while kill -0 -- -"$LPID" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
        kill -KILL -- -"$LPID" 2>/dev/null || true
    fi
    [ -z "$BPID" ] || kill "$BPID" 2>/dev/null || true
    pkill -f "$T/stubfm.py" 2>/dev/null || true
    [ -z "$XPID" ] || kill "$XPID" 2>/dev/null || true
    if [ -n "${SHOW_IN_FOLDER_KEEP:-}" ]; then echo "kept $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT INT TERM

mkdir -p "$T/home/.config" "$T/home/.local/share" "$T/home/.cache" "$T/run" "$T/services"
chmod 700 "$T/run"
LIB="$T/home/Music/Ableton/User Library"
NAME='Bass C#2, 100% open.wav'
mkdir -p "$LIB"
python3 -I - "$LIB/$NAME" <<'PY'
import sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"\0" * 4 * 44100 * 2)
w.close()
PY

# The stub file manager: started by the bus on the first ShowItems, writes
# each request as "<uri count> <uris...> | <startup id>".
cat > "$T/stubfm.py" <<'PY'
import sys
from gi.repository import Gio, GLib
XML = """<node><interface name='org.freedesktop.FileManager1'>
<method name='ShowItems'><arg type='as' direction='in'/><arg type='s' direction='in'/></method>
</interface></node>"""
info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
def call(conn, sender, path, iface, method, params, inv):
    uris, startup = params.unpack()
    with open(sys.argv[1], "a") as f:
        f.write("%d %s | %s\n" % (len(uris), " ".join(uris), startup))
    inv.return_value(None)
def connected(conn, name): # before the name is taken, so the call that started it finds the object
    conn.register_object("/org/freedesktop/FileManager1", info, call, None, None)
Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.FileManager1", 0, connected, None, None)
GLib.MainLoop().run()
PY
# Under a name other than the bus name, as file managers often install it.
printf '[D-BUS Service]\nName=org.freedesktop.FileManager1\nExec=/usr/bin/python3 -I %s %s\n' \
    "$T/stubfm.py" "$T/uris.log" > "$T/services/org.example.StubFileManager.service"
cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$T/bus</listen>
  <servicedir>$T/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
env -i PATH=/usr/bin:/bin HOME="$T/home" dbus-daemon --config-file="$T/bus.conf" --nofork > "$T/bus.log" 2>&1 &
BPID=$!
i=0
until [ -S "$T/bus" ]; do
    i=$((i + 1)); [ $i -lt 100 ] || { echo "FAIL: the session bus did not start"; exit 1; }
    sleep 0.1
done

n=90
while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do
    n=$((n + 1)); [ $n -lt 120 ] || { echo "FAIL: no free display"; exit 1; }
done
DPY=:$n
Xvfb $DPY -screen 0 1920x1080x24 -nolisten tcp > "$T/xvfb.log" 2>&1 &
XPID=$!
i=0
until [ -e /tmp/.X11-unix/X$n ]; do
    i=$((i + 1)); [ $i -lt 100 ] || { echo "FAIL: Xvfb did not start"; exit 1; }
    sleep 0.1
done

(cd "$APP" && exec setsid env -i PATH=/usr/bin:/bin DISPLAY=$DPY HOME="$T/home" \
    XDG_CONFIG_HOME="$T/home/.config" XDG_DATA_HOME="$T/home/.local/share" \
    XDG_CACHE_HOME="$T/home/.cache" XDG_RUNTIME_DIR="$T/run" LANG=C.UTF-8 \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$T/bus" \
    LIVE_SHIM="$SHIM/libmenufix.so" MENUFIX_DEBUG="$T/menufix.trace" \
    LIVE_DATA_DIR="$T/home" \
    nice -n 19 unshare -c -n ./AbletonLive12 > "$T/live.out" 2>&1) &
LPID=$!

export DISPLAY=$DPY
win=
i=0
while [ -z "$win" ]; do
    i=$((i + 1)); [ $i -lt 300 ] || { echo "FAIL: Live's main window did not appear in 5 minutes"; exit 1; }
    kill -0 "$LPID" 2>/dev/null || { echo "FAIL: Live exited during start-up"; tail -5 "$T/live.out"; exit 1; }
    win=$(xdotool search --onlyvisible --name '^Untitled' 2>/dev/null | head -1) || true
    sleep 1
done
# Let start-up settle and dismiss the authorisation prompt ("Authorize later").
sleep 15
xdotool mousemove 768 605 click 1
sleep 3
shot() { import -window root "$T/$1.png"; }
shot start

# Positions on the 1920x1080 display (DPI 96, Live's default zoom).
PLACE_X=60 PLACE_Y=486   # "User Library" under Places, the sidebar scrolled to its end
FILE_X=200 FILE_Y=144    # the WAV, the only item in the User Library
ITEM_DX=55 ITEM_DY=167   # "Show in Folder" in the menu, from the right-click point
if [ -n "${SHOW_IN_FOLDER_CALIBRATE:-}" ]; then # leaves Live up on $DISPLAY until $T/stop exists
    echo "calibrate: DISPLAY=$DISPLAY T=$T"; SHOW_IN_FOLDER_KEEP=1
    while [ ! -e "$T/stop" ]; do sleep 1; done; exit 0
fi

xdotool mousemove $PLACE_X $PLACE_Y
sleep 1
for k in 1 2 3 4 5 6 7 8 9 10; do xdotool click 5; sleep 0.2; done
sleep 1
xdotool click 1
sleep 2
xdotool mousemove $FILE_X $FILE_Y click 1
sleep 1
shot browser
menu() { # opens the file's context menu; $1: choose Show in Folder
    xdotool mousemove $FILE_X $FILE_Y click 3
    sleep 1
    if [ "$1" = choose ]; then
        xdotool mousemove $((FILE_X + ITEM_DX)) $((FILE_Y + ITEM_DY)) # Live takes a click on a row it has seen hovered
        sleep 0.5
        xdotool click 1
    else # a left click outside the menu closes it (Escape does not reach it without a window manager)
        xdotool mousemove $FILE_X $FILE_Y click 1
    fi
    sleep 1
}
menu open; shot menu
menu choose
wait_uris() { # $1 lines in the stub's log within 10 s
    i=0
    while [ "$(cat "$T/uris.log" 2>/dev/null | wc -l)" -lt "$1" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
}
wait_uris 1

# One URI, which a URI parser turns back into the file's exact path (so '#',
# ',' and '%' are encoded), and an empty startup id.
decodes() {
    python3 -I - "$1" "$LIB/$NAME" <<'PY2'
import sys, urllib.parse
line, path = sys.argv[1], sys.argv[2]
count, rest = line.split(" ", 1)
uri, _, startup = rest.rpartition(" | ")
u = urllib.parse.urlsplit(uri)
ok = (count == "1" and u.scheme == "file" and not u.query and not u.fragment
      and urllib.parse.unquote(u.path) == path and startup == "")
sys.exit(0 if ok else 1)
PY2
}
fail=0
first=$(head -1 "$T/uris.log" 2>/dev/null || true)
if [ -n "$first" ] && decodes "$first"; then echo "PASS: the file manager was asked to show the file"; else
    echo "FAIL: the file manager got '${first:-nothing}' for $LIB/$NAME"; fail=1; fi

for k in 1 2 3 4 5 6 7 8 9; do menu open; done
menu choose
menu choose
wait_uris 3
if kill -0 "$LPID" 2>/dev/null && [ "$(cat "$T/uris.log" 2>/dev/null | wc -l)" -eq 3 ] \
    && [ "$(sort -u "$T/uris.log" | wc -l)" -eq 1 ]; then
    echo "PASS: Live still runs after repeated menus, three identical requests"
else
    echo "FAIL: after repeated menus Live runs: $(kill -0 "$LPID" 2>/dev/null && echo yes || echo no), requests:"
    cat "$T/uris.log" 2>/dev/null || true; fail=1
fi
shot end
grep -h showinfolder "$T/menufix.trace" 2>/dev/null | head -3 || true
[ "$fail" = 0 ] || exit 1
echo "PASS"
