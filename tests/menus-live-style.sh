#!/bin/sh
# Drop-down menus in the style of Live's own menus. Starts the
# installed Live with the given shim on its own Xvfb display, with a throwaway
# HOME, no network and a private session bus, in Live's default theme. Opens
# Live's browser context menu on a file and the shim's File menu, each with
# its first row highlighted, and compares the two screenshots:
#   colours: menu body, frame and text, each channel within 3;
#   size:    row height (the hover plate) within 1 px, capital height of the
#            first row's first letter within 1 px.
# Runs Live (about two minutes), so it is on demand, not in tests/verify.sh.
# Never touches the real ~/.config/Ableton: everything lives in a temp folder.
# Usage: tests/menus-live-style.sh [shim folder] [install folder]
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
    [ -z "$XPID" ] || kill "$XPID" 2>/dev/null || true
    if [ -n "${SHOW_IN_FOLDER_KEEP:-}" ]; then echo "kept $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT INT TERM

mkdir -p "$T/home/.config" "$T/home/.local/share" "$T/home/.cache" "$T/run" "$T/services"
chmod 700 "$T/run"
LIB="$T/home/Music/Ableton/User Library"
NAME='Bass.wav'
mkdir -p "$LIB"
python3 -I - "$LIB/$NAME" <<'PY'
import sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"\0" * 4 * 44100 * 2)
w.close()
PY

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
# 96 dpi: scale 1 (Xvfb reports 100 dpi by default, and Live passes the exact
# scale to Qt, so the menus would be drawn at 1.04x).
Xvfb $DPY -screen 0 1920x1080x24 -dpi 96 -nolisten tcp > "$T/xvfb.log" 2>&1 &
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
xdotool mousemove 736 580 click 1
sleep 3
shot() { import -window root "$T/$1.png"; }
shot start

# Positions on the 1920x1080 display at 96 dpi, Live's default zoom (those in
# tests/show-in-folder-live.sh are for 100 dpi, 1.04 times these).
PLACE_X=58 PLACE_Y=467   # "User Library" under Places, the sidebar scrolled to its end
FILE_X=192 FILE_Y=138    # the WAV, the only item in the User Library
xdotool mousemove $PLACE_X $PLACE_Y
sleep 1
for k in 1 2 3 4 5 6 7 8 9 10; do xdotool click 5; sleep 0.2; done
sleep 1
xdotool click 1
sleep 2
xdotool mousemove $FILE_X $FILE_Y click 1
sleep 1

# Live's context menu: it opens below the pointer; the pointer goes to its first row.
xdotool mousemove $FILE_X $FILE_Y click 3
sleep 1
xdotool mousemove $((FILE_X + 60)) $((FILE_Y + 9))
sleep 1
shot live
xdotool mousemove $((FILE_X + 60)) $((FILE_Y + 25)) # the second row
sleep 1
shot live2
xdotool mousemove $FILE_X $FILE_Y click 1 # closes it (Escape does not reach it without a window manager)
sleep 1

# The shim's File menu, first item in the menu bar; the arrow keys go to its rows.
menuTop() {
python3 -I - "$T" <<'PY'
import subprocess, sys
raw = subprocess.run(["import", "-window", "root", "-depth", "8", "ppm:-"], capture_output=True).stdout
head, _, data = raw.partition(b"\n255\n")
w = int(head.split()[1])
px = lambda x, y: data[(y * w + x) * 3:(y * w + x) * 3 + 3]
band = px(60, 17) # in the bar, under its titles
y = 17
while y < 200 and px(60, y) == band:
    y += 1
print(y) # the first change under the bar: the open menu's top frame line
PY
}
# With File closed the scan meets Live's own view under the bar; the open
# menu's frame is higher. A click can go to closing Live's own menu instead:
# click the title again until the menu shows.
closed=$(menuTop)
xdotool mousemove 20 9 click 1
sleep 1
top=$closed i=0
while [ "$top" -ge "$closed" ] && [ $i -lt 6 ]; do
    top=$(menuTop)
    [ "$top" -lt "$closed" ] || { xdotool mousemove 20 9 click 1; sleep 1; }
    i=$((i + 1))
done
[ "$top" -lt "$closed" ] || { shot nomenu; echo "FAIL: the shim's File menu did not open (scan: $top, closed: $closed)"; exit 1; }
# The arrow keys select its rows (the pointer does not reach a menu in this
# headless session); a selected row has the same plate as a hovered one.
xdotool key Down
sleep 1
shot shim
xdotool key Down # the second row
sleep 1
shot shim2
xdotool key Escape
sleep 1

status=0
python3 -I - "$T/live" $((FILE_X + 60)) $((FILE_Y + 9)) "$T/shim" 60 $((top + 9)) <<'PY' || status=1
import subprocess, sys

def load(path):
    raw = subprocess.run(["magick", path, "-depth", "8", "ppm:-"], capture_output=True).stdout
    head, _, data = raw.partition(b"\n255\n")
    w, h = (int(v) for v in head.split()[1:3])
    def px(x, y):
        if not (0 <= x < w and 0 <= y < h):
            return (0, 0, 0)
        return tuple(data[(y * w + x) * 3:(y * w + x) * 3 + 3])
    return px

def lum(c):
    return sum(c) / 3

def commonest(px, xs, y):
    counts = {}
    for xx in xs:
        c = px(xx, y)
        counts[c] = counts.get(c, 0) + 1
    return max(counts, key=counts.get)

def plate_top(px, x, y, plate):
    while px(x, y - 1) == plate:
        y -= 1
    return y

def measure(stem, x, y):
    # stem.png: the pointer on the first row (x, y); stem2.png: on the second (x, y + 16).
    px, px2 = load(stem + ".png"), load(stem + "2.png")
    # The plate: the commonest colour along the row (letters cover less of it).
    plate = commonest(px, range(x - 40, x + 60), y)
    # A line of the row with no letters on it: the one most covered by the plate.
    clear = max(range(y - 7, y + 8), key=lambda yy: sum(px(xx, yy) == plate for xx in range(x - 40, x + 60)))
    # Left edge of the plate: walk left along that line to the frame.
    left = x
    while left > 0 and px(left - 1, clear) == plate:
        left -= 1
    frame = px(left - 1, clear)
    # Row pitch: how far the plate moves from the first row to the second.
    top1 = plate_top(px, left + 1, clear, plate)
    plate2 = commonest(px2, range(x - 40, x + 60), clear + 16)
    top2 = plate_top(px2, left + 1, clear + 16, plate2)
    row = top2 - top1
    # The second capture must show the same plate one row down, gone from the first row.
    if plate2 != plate or px2(left + 1, clear) == plate:
        row = 0
    if row <= 0: # no plate that moves by a row: nothing more to measure
        return dict(body=(0, 0, 0), frame=frame, text=(0, 0, 0), row=row, cap=0)
    # Body: the commonest colour of four rows under the second, left part.
    rows = range(top1 + 2 * row, top1 + 6 * row)
    counts = {}
    for yy in rows:
        for xx in range(left + 1, left + 120):
            c = px(xx, yy)
            counts[c] = counts.get(c, 0) + 1
    body = max(counts, key=counts.get)
    # Text: the pixel farthest from the body in the label area of those rows,
    # leaving out separator lines (the frame colour across the row).
    text = body
    for yy in rows:
        if px(left + 1, yy) != body:
            continue
        for xx in range(left + 10, left + 120):
            c = px(xx, yy)
            if abs(lum(c) - lum(body)) > abs(lum(text) - lum(body)):
                text = c
    # Capital height: the first letter of the first row (its first 3 ink columns).
    inked = lambda xx, yy: abs(lum(px(xx, yy)) - lum(plate)) > 60
    first = next((xx for xx in range(left + 4, left + 60)
                  if any(inked(xx, yy) for yy in range(top1, top1 + row))), left + 4)
    ink = [yy for yy in range(top1, top1 + row) for xx in range(first, first + 3) if inked(xx, yy)]
    cap = max(ink) - min(ink) + 1 if ink else 0
    return dict(body=body, frame=frame, text=text, row=row, cap=cap)

hexc = lambda c: "#%02x%02x%02x" % c
live = measure(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
shim = measure(sys.argv[4], int(sys.argv[5]), int(sys.argv[6]))
fail = 0
for key in ("body", "frame", "text"):
    ok = all(abs(a - b) <= 3 for a, b in zip(live[key], shim[key]))
    print("  %s %s: Live %s, shim %s" % ("ok  " if ok else "FAIL", key, hexc(live[key]), hexc(shim[key])))
    fail += not ok
for key, what in (("row", "row pitch"), ("cap", "capital height")):
    ok = abs(live[key] - shim[key]) <= 1
    print("  %s %s: Live %d px, shim %d px" % ("ok  " if ok else "FAIL", what, live[key], shim[key]))
    fail += not ok
print("FAIL: %d checks" % fail if fail else "PASS: the shim's menu matches Live's own menu")
sys.exit(1 if fail else 0)
PY
# The bar and the drop-down draw in Live's font (the shim's trace, from each
# widget's resolved font).
for what in bar drop-down; do
    if grep -q "^nativemenu: $what in Live's menu font 1$" "$T/menufix.trace"; then
        echo "  ok   $what in Live's menu font"
    else
        echo "FAIL: $what not in Live's menu font"; status=1
    fi
done
exit $status
