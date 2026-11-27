#!/bin/sh
# Drop-down lists in a modal dialog (File > Export Audio/Video) take the mouse.
# Starts Live with the given shim on its own Xvfb display, with a throwaway HOME
# and data folder, a private /tmp (so it also runs while another Live is open),
# no network, no sound or USB devices and a private session bus. Opens File > Export
# Audio/Video, then for the Rendered Track list and the Sample Rate list:
# opens the list, hovers its second or fourth row and clicks it. Each click
# must reach the list (the shim's trace logs "press x,y inside=1") and change
# the chooser's text (its pixels differ from before). Then a press outside an
# open list must close it, and after Cancel a list in the main window (track 1's
# MIDI To) must still take a click.
# Runs Live (about 20 s, up to five minutes), so it is on demand, not in tests/verify.sh.
# Copies Live's authorisation (Unlock.json) into the temp folder, read only.
# Usage: tests/export-dropdown.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12; neither under /tmp.
#        While that Live is running, pass a copy: cp -a ~/.local/share/ableton-live-12 ~/.cache/live-copy)
#        EXPORT_DROPDOWN_KEEP=1 keeps the temp folder (screenshots, logs).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=$(cd "${2:-$HOME/.local/share/ableton-live-12}" && pwd)
APP=$PREFIX/push3_opt/push3
UNLOCK="$HOME/.local/share/Ableton/Live 12.4.7/Unlock/Unlock.json"
# Live runs with a private /tmp, so nothing it loads may live under /tmp.
case "$SHIM/ $PREFIX/" in
    /tmp/*|*" /tmp/"*) echo "FAIL: the shim and install folders must not be under /tmp (Live gets a private /tmp)"; exit 1 ;;
esac
[ -f "$SHIM/libmenufix.so" ] || { echo "FAIL: no libmenufix.so in $SHIM"; exit 1; }
[ -x "$APP/AbletonLive12" ] || { echo "FAIL: no Live at $APP"; exit 1; }
[ -f "$UNLOCK" ] || { echo "FAIL: Live is not authorised ($UNLOCK)"; exit 1; }
for tool in Xvfb xdotool import identify dbus-daemon unshare; do
    command -v $tool >/dev/null || { echo "FAIL: $tool is needed"; exit 1; }
done
# Live writes sockets into its install folder: never share one with a running copy.
if pgrep -u "$(id -u)" -f "$APP/(AbletonLive12|Live)( |\$)" >/dev/null; then
    echo "FAIL: Live from $PREFIX is already running; close it or pass a copy"; exit 1
fi

# Not under /tmp: the private /tmp below would hide it from Live.
mkdir -p "${XDG_CACHE_HOME:-$HOME/.cache}"
T=$(mktemp -d "${XDG_CACHE_HOME:-$HOME/.cache}/export-dropdown.XXXXXX")
XPID= LPID= BPID=
cleanup() {
    if [ -n "$LPID" ]; then # Live and its helpers share the session started by setsid
        kill -TERM -- -"$LPID" 2>/dev/null || true
        i=0
        while kill -0 -- -"$LPID" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
        kill -KILL -- -"$LPID" 2>/dev/null || true
    fi
    [ -z "$BPID" ] || kill "$BPID" 2>/dev/null || true
    [ -z "$XPID" ] || kill "$XPID" 2>/dev/null || true
    if [ -n "${EXPORT_DROPDOWN_KEEP:-}" ]; then echo "kept $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT INT TERM

mkdir -p "$T/home/.config" "$T/home/.local/share" "$T/home/.cache" "$T/run" "$T/services" \
    "$T/tmp/.X11-unix" "$T/empty"
chmod 700 "$T/run"
U="$T/home/.local/share/Ableton/Live 12.4.7/Unlock"
mkdir -p "$U"; cp "$UNLOCK" "$U/"

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
n=90
while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do n=$((n + 1)); done
DPY=:$n
Xvfb $DPY -screen 0 1920x1080x24 -dpi 96 -nolisten tcp > "$T/xvfb.log" 2>&1 &
XPID=$!
i=0
until [ -e /tmp/.X11-unix/X$n ] && [ -S "$T/bus" ]; do
    i=$((i + 1)); [ $i -lt 100 ] || { echo "FAIL: Xvfb or the session bus did not start"; exit 1; }
    sleep 0.1
done

# Mount namespace (as root of a user namespace): private /tmp holding only the
# X socket, empty /dev/snd and /dev/bus/usb, fresh /dev/shm; then Live runs as
# the user again, with no network.
export T
(cd "$APP" && exec setsid env -i PATH=/usr/bin:/bin DISPLAY=$DPY HOME="$T/home" \
    XDG_CONFIG_HOME="$T/home/.config" XDG_DATA_HOME="$T/home/.local/share" \
    XDG_CACHE_HOME="$T/home/.cache" XDG_RUNTIME_DIR="$T/run" LANG=en_US.UTF-8 \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$T/bus" \
    LIVE_SHIM="$SHIM/libmenufix.so" MENUFIX_DEBUG="$T/menufix.trace" LIVE_DATA_DIR="$T/home" T="$T" \
    nice -n 19 unshare -r -m -n sh -c '
        mount --bind /tmp/.X11-unix "$T/tmp/.X11-unix" &&
        mount --rbind "$T/tmp" /tmp &&
        mount --bind "$T/empty" /dev/snd &&
        mount --bind "$T/empty" /dev/bus/usb &&
        mount -t tmpfs tmpfs /dev/shm &&
        exec unshare -U --map-user='"$(id -u)"' --map-group='"$(id -g)"' ./AbletonLive12' \
    > "$T/live.out" 2>&1) &
LPID=$!
export DISPLAY=$DPY

i=0
until xdotool search --name '^Untitled' >/dev/null 2>&1; do
    i=$((i + 1)); [ $i -lt 300 ] || { echo "FAIL: Live's window did not appear"; tail -5 "$T/live.out"; exit 1; }
    kill -0 "$LPID" 2>/dev/null || { echo "FAIL: Live exited"; tail -5 "$T/live.out"; exit 1; }
    sleep 1
done
sleep 5

# The dialog opens centred on the 1920x1080 screen, so its controls sit at
# fixed points (no window manager, 96 dpi).
xdotool mousemove 600 650 key ctrl+shift+r
sleep 3
fails=0

# pick <name> <chooser x y> <row x y> <chooser text crop, inside its border>
pick() {
    name=$1 cx=$2 cy=$3 rx=$4 ry=$5 crop=$6
    import -window root -crop $crop "$T/$name-before.png"
    before=$(identify -format %# "$T/$name-before.png") # pixels only: PNG files carry a timestamp
    xdotool mousemove $cx $cy click 1
    sleep 1.2
    import -window root "$T/$name-open.png"
    xdotool mousemove $rx $((ry - 4)); sleep 0.3; xdotool mousemove $rx $ry; sleep 0.8
    import -window root "$T/$name-hover.png"
    xdotool click 1
    sleep 1.2
    import -window root -crop $crop "$T/$name-after.png"
    after=$(identify -format %# "$T/$name-after.png")
    if grep -q "^press $rx,$ry inside=1" "$T/menufix.trace"; then
        reached=1
    else
        reached=0
    fi
    if [ $reached = 1 ] && [ "$before" != "$after" ]; then
        echo "ok   $name: the click on the list reached it and changed the choice"
    else
        echo "FAIL $name: click at $rx,$ry reached the list: $reached; choice changed: $([ "$before" != "$after" ] && echo 1 || echo 0)"
        fails=$((fails + 1))
    fi
    # The hovered row must light up: it differs between the open and hover pictures
    row="30x8+$((rx - 15))+$((ry - 4))"
    if [ "$(identify -format %# "$T/$name-open.png[$row]")" = "$(identify -format %# "$T/$name-hover.png[$row]")" ]; then
        echo "FAIL $name: the hovered row at $rx,$ry did not light up"
        fails=$((fails + 1))
    fi
}
pick rendered-track 668 167 665 199 60x8+618+163 # Main -> All Individual Tracks
pick sample-rate 683 370 695 432 28x8+678+366    # 44100 -> 48000

if [ $fails -gt 0 ]; then
    echo "trace:"; grep '^press\|^show' "$T/menufix.trace" | tail -8 | sed 's/^/  /'
    echo "FAIL: $fails of 2 lists in the Export dialog ignored the mouse"
    exit 1
fi

# A press outside an open list closes it: the list's area looks as it did
# before it opened. The press lands on the Normalize label, which does nothing.
area=111x107+615+176
import -window root -crop $area "$T/outside-before.png"
xdotool mousemove 668 167 click 1; sleep 1.2
import -window root -crop $area "$T/outside-open.png"
xdotool mousemove 500 325 click 1; sleep 1.2
import -window root -crop $area "$T/outside-after.png"
closed=$(identify -format %# "$T/outside-before.png")
if [ "$(identify -format %# "$T/outside-open.png")" = "$closed" ]; then
    echo "FAIL outside: the Rendered Track list did not open"; exit 1
fi
if [ "$(identify -format %# "$T/outside-after.png")" != "$closed" ]; then
    echo "FAIL outside: a press outside the open list left it open"; exit 1
fi
echo "ok   outside: a press outside the open list closed it"

# After the dialog is cancelled, a list in the main window still takes the
# mouse (the list keeps the closed dialog as its transient parent).
xdotool mousemove 635 601 click 1; sleep 1.5
pick main-midi-to 366 302 360 338 60x8+327+298  # track 1 MIDI To: No Output -> 2-MIDI
[ $fails -eq 0 ] || { echo "FAIL: the main window's list ignored the mouse after the dialog closed"; exit 1; }
echo "PASS: drop-down lists in the Export dialog take hover and clicks"
