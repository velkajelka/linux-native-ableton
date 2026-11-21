#!/bin/sh
# A file dragged in from outside Live shows its clip in the arrangement while
# the drag is over it.
# Starts the installed Live on its own Xvfb display with a throwaway HOME and
# no network, then drags a silent WAV over the 3 Audio lane with
# tests/xdnd-source.c, shaped like a Wayland file manager's drag through
# Xwayland. Checks, by comparing screenshots of the lane:
#   preview: the lane changes during the drag (red before the fix);
#   leave:   leaving the window restores the lane;
#   drop:    a drop still places the clip.
# Runs Live (about two minutes), so it is on demand, not in tests/verify.sh.
# Never touches the real ~/.config/Ableton: everything lives in a temp folder.
# Usage: tests/drag-preview.sh [shim folder] [install folder]
#        (defaults: the repo's shim/, ~/.local/share/ableton-live-12)
#        DRAG_PREVIEW_KEEP=1 keeps the temp folder (screenshots, Live's log).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$(cd "${1:-$REPO/shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
APP=$PREFIX/push3_opt/push3
[ -f "$SHIM/libmenufix.so" ] || { echo "FAIL: no libmenufix.so in $SHIM"; exit 1; }
[ -x "$APP/AbletonLive12" ] || { echo "FAIL: no installed Live at $APP"; exit 1; }
for tool in Xvfb xdotool import compare cc unshare python3; do
    command -v $tool >/dev/null || { echo "FAIL: $tool is needed"; exit 1; }
done

# Live runs once per user: a second copy hands over to the first and never
# shows a window here.
if pgrep -u "$(id -u)" -f 'push3_opt/push3/(AbletonLive12|Live)( |$)' >/dev/null; then
    echo "FAIL: Live is already running; close it and run this again"; exit 1
fi

T=$(mktemp -d)
XPID= LPID=
cleanup() {
    if [ -n "$LPID" ]; then # Live and its helpers share the session started by setsid
        kill -TERM -- -"$LPID" 2>/dev/null || true
        i=0
        while kill -0 -- -"$LPID" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
        kill -KILL -- -"$LPID" 2>/dev/null || true
    fi
    [ -z "$XPID" ] || kill "$XPID" 2>/dev/null || true
    if [ -n "${DRAG_PREVIEW_KEEP:-}" ]; then echo "kept $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT INT TERM

cc -O1 -o "$T/xdnd-source" "$REPO/tests/xdnd-source.c" -lX11 > "$T/build.log" 2>&1 ||
    { echo "FAIL: xdnd-source did not build"; tail -10 "$T/build.log"; exit 1; }
python3 -I - "$T/silent.wav" <<'PY'
import sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"\0" * 4 * 44100 * 16)
w.close()
PY

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

mkdir -p "$T/home/.config" "$T/home/.local/share" "$T/home/.cache" "$T/run"
chmod 700 "$T/run"
(cd "$APP" && exec setsid env -i PATH=/usr/bin:/bin DISPLAY=$DPY HOME="$T/home" \
    XDG_CONFIG_HOME="$T/home/.config" XDG_DATA_HOME="$T/home/.local/share" \
    XDG_CACHE_HOME="$T/home/.cache" XDG_RUNTIME_DIR="$T/run" LANG=C.UTF-8 \
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
# Let start-up settle and dismiss the authorisation prompt ("Authorize
# later"). Then switch to the Arrangement with Tab, giving the main window the
# keyboard first (Xvfb has no window manager); Live can miss the first try.
# Above the tracks the Arrangement has its grey ruler, the Session coloured
# track titles.
sleep 15
xdotool mousemove 768 605 click 1
sleep 3
px() { magick "$T/arrangement.png" -crop 1x1+$1 -format '%[hex:p{0,0}]' info:; }
i=0
while :; do
    import -window root "$T/arrangement.png"
    [ "$(px 340+75)$(px 500+75)" != A3A3A3A3A3A3 ] || break
    i=$((i + 1))
    [ $i -le 5 ] || { echo "FAIL: could not switch Live to the Arrangement (DRAG_PREVIEW_KEEP=1 keeps arrangement.png)"; exit 1; }
    xdotool mousemove 1100 500 click 1
    sleep 1
    xdotool windowfocus --sync "$win" key Tab
    sleep 3
done

lane() { magick "$1" -crop 280x80+335+310 +repage "$2"; }
differs() { # pixels that differ between two lane crops (Xvfb renders the same twice)
    compare -metric AE "$1" "$2" null: 2>&1 | cut -d' ' -f1 | cut -d. -f1 || true
}
uri=file://$T/silent.wav
fail=0

"$T/xdnd-source" "$win" "$uri" 380 340 480 350 30 "$T/leave" leave > "$T/leave.log" 2>&1 ||
    { echo "FAIL: drag did not complete"; cat "$T/leave.log"; exit 1; }
import -window root "$T/leave-after.png"
for s in before during after; do lane "$T/leave-$s.png" "$T/lane-leave-$s.png"; done
during=$(differs "$T/lane-leave-before.png" "$T/lane-leave-during.png")
after=$(differs "$T/lane-leave-before.png" "$T/lane-leave-after.png")
echo "$(grep 'moves accepted' "$T/leave.log" | cut -c9-); lane pixels changed: during $during, after leaving $after"
if [ "$during" -gt 40 ]; then echo "PASS: preview shown during the drag"; else
    echo "FAIL: nothing shown in the lane during the drag"; fail=1; fi
if [ "$after" -lt 20 ]; then echo "PASS: nothing left after leaving"; else
    echo "FAIL: the lane changed after the drag left"; fail=1; fi

"$T/xdnd-source" "$win" "$uri" 380 340 480 350 30 "$T/drop" drop > "$T/drop.log" 2>&1 ||
    { echo "FAIL: drop did not complete"; cat "$T/drop.log"; exit 1; }
sleep 3
import -window root "$T/drop-after.png"
lane "$T/drop-before.png" "$T/lane-drop-before.png"
lane "$T/drop-after.png" "$T/lane-drop-after.png"
dropped=$(differs "$T/lane-drop-before.png" "$T/lane-drop-after.png")
if grep -q dropped "$T/drop.log" && [ "$dropped" -gt 300 ]; then echo "PASS: drop placed the clip"; else
    echo "FAIL: drop did not place a clip (lane pixels changed: $dropped)"; fail=1; fi

kill -0 "$LPID" 2>/dev/null || { echo "FAIL: Live is no longer running"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "PASS"
