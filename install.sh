#!/usr/bin/env bash
# Installs Ableton Live 12 (or Live 11) as a Linux desktop app: the Linux build
# from the Push 3 recovery image, the content the image lacks from the Windows
# Suite installer (Core Library, lessons, Live 12's high-quality stem models,
# cursors, icon, edition), and the shim and launcher next to this script. Live's own files
# stay as Ableton made them. Live then runs like any Linux program: settings
# in ~/.config/Ableton and ~/.local/share/Ableton, libraries in the desktop's
# Music folder (<Music>/Ableton). Writes uninstall.sh into the install folder.
# A run that stops (an error, a full disk, the window closed) can be started
# again: it continues after the last finished part. What the tools report goes
# to install.log in the install folder.
#
# usage: install.sh [--version 12|11] [--edition N] [--prefix DIR] [--portable] [--cpu auto|avx512|avx2]
#   Put the Push 3 recovery image and the Windows Live Suite installer in this
#   script's folder: Live 12: Push3-RecoveryImage-2.4.7.zip and
#   ableton_live_suite_12.4.7_64.zip; Live 11: Push3-RecoveryImage-1.2.23.zip and
#   ableton_live_suite_11.3.43_64.zip (both pairs may be there).
#   No options are needed: on a terminal it asks for the version, the edition, a
#   normal or portable installation and the install folder, each with a default.
#   --version     12 (Live 12.4.7) or 11 (Live 11.3.43); asked when not given
#                 (12 by default), or the one whose files are there
#   --edition N   the edition your licence is for: 1 Suite, 2 Standard, 3 Intro,
#                 4 Lite (Live 12 only); Live accepts only a licence for this edition.
#                 Asked when not given (Suite by default, or what an earlier
#                 installation in the folder had)
#   --prefix DIR  install folder for the program (default ~/.local/share/ableton-live-12,
#                 Live 11: ableton-live-11)
#   --portable    everything in one folder next to this script (AbletonLive12-portable or
#                 AbletonLive11-portable, or --prefix): program, settings, licence,
#                 libraries; no menu entry
#   --cpu         auto (default): the AVX-512 version on CPUs with AVX-512, else the
#                 AVX2 version (with the AVX-512 emulator); portable: both
#   --update [DIR] update an installation (DIR, else the one found) to this version: only
#                 this project's files are replaced; needs no Push 3 image or Suite installer.
#                 Without options on a terminal, install.sh looks for installations first
#                 and offers to update one.
set -euo pipefail

# Started from a file manager there is no terminal to show the progress in:
# run again inside one, and keep its window open at the end.
if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${LIVE_INSTALLER_TERMINAL:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    self=$(readlink -f "$0")
    run=(env LIVE_INSTALLER_TERMINAL=1 bash -c 'bash "$0" "$@"; status=$?; echo; read -r -p "Press Enter to close this window. " _ </dev/tty; exit $status' "$self" "$@")
    case "${XDG_CURRENT_DESKTOP:-}" in
        *KDE*) order=(konsole) ;;
        *GNOME* | *Unity* | *Pantheon*) order=(kgx ptyxis gnome-terminal) ;;
        *Cinnamon* | *X-Cinnamon*) order=(gnome-terminal) ;;
        *XFCE*) order=(xfce4-terminal) ;;
        *MATE*) order=(mate-terminal) ;;
        *LXQt*) order=(qterminal) ;;
        *) order=() ;;
    esac
    order+=(x-terminal-emulator konsole kgx ptyxis gnome-terminal xfce4-terminal mate-terminal qterminal
            tilix alacritty kitty foot wezterm xterm)
    for term in "${order[@]}"; do
        command -v "$term" >/dev/null || continue
        case $term in
            kgx | ptyxis | gnome-terminal) exec "$term" -- "${run[@]}" ;;
            xfce4-terminal | mate-terminal) exec "$term" -x "${run[@]}" ;;
            kitty | foot) exec "$term" "${run[@]}" ;;
            wezterm) exec "$term" start -- "${run[@]}" ;;
            *) exec "$term" -e "${run[@]}" ;;
        esac
    done
    message="No terminal program found. Open a terminal in $(dirname "$self") and run ./install.sh"
    command -v kdialog >/dev/null && exec kdialog --error "$message"
    command -v zenity >/dev/null && exec zenity --error --text="$message"
    command -v notify-send >/dev/null && exec notify-send "Ableton Live installer" "$message"
    exit 1
fi

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
PREFIX=""
PREFIX_SET=0 PORTABLE=0 CPU_CHOICE=auto LIVE_MAJOR="" EDITION="" UPDATE=0 UPDATE_DIR=""
# Qt's xcb helpers, next to Live's Qt (its RUNPATH $ORIGIN wins over Live's RPATH).
XCB_LIBS=(libxcb-icccm.so.4 libxcb-image.so.0 libxcb-util.so.1 libxcb-keysyms.so.1 libxcb-render-util.so.0
          libxcb-xinerama.so.0 libxcb-xinput.so.0 libxcb-xkb.so.1)

# set_version: what differs between the two Lives (LIVE_MAJOR 12 or 11).
#   LIVE_SHA256       the Live binary this version's fixes were made for
#   NEEDED_GB         space while installing; PROGRAM_DONE_GB once the program is in;
#                     AFTER_GB what stays
#   PUSH3_BYTES, SUITE_BYTES  sizes for the progress bars where the tools report none
#                     (another version only makes the bar less exact)
#   COMPAT            libraries from the image's /usr/lib that Live links and desktop
#                     systems lack (compat_lib); FALLBACK: Qt's xcb helpers and libnsl,
#                     used only when the system has none (copy_lib)
#   SUITE_PARTS       the Suite installer's files that are needed
#   PUSH_GLOB, SUITE_GLOB (with *_SKIP: names of the other version's files)
set_version() {
    case $LIVE_MAJOR in
    12)
        LIVE_FULL=12.4.7 PUSH_VERSION=2.4.7
        LIVE_SHA256=8f9052719b35d6add174c2fe9a61897c7115717bc4d3d6623697881976251dc0
        NEEDED_GB=25 PROGRAM_DONE_GB=13 AFTER_GB=7
        PUSH3_BYTES=3655802632  # /opt/push3
        SUITE_BYTES=3375249763  # SUITE_PARTS
        # FFmpeg 5, ICU 70, Theora
        COMPAT=(libavcodec.so.59.18.100 libavformat.so.59.16.100 libavutil.so.57.17.100 libswresample.so.4.3.100
                libswscale.so.6.4.100 libicudata.so.70.1 libicui18n.so.70.1 libicuuc.so.70.1
                libtheoradec.so.1.1.4 libtheoraenc.so.1.1.2)
        NSL=libnsl.so.3.0.0
        # innoextract --include (Live 12.4's installer is Inno Setup 6.4)
        SUITE_PARTS=("/app/Resources/Core Library" "/app/Resources/Builtin Lessons" "/app/Resources/Builtin/Models"
                     "/app/Resources/Cursors" "/app/Resources/Icons" "/app/Program/Installation.cfg")
        PUSH_GLOB="Push3-RecoveryImage-*.zip" PUSH_SKIP="Push3-RecoveryImage-1.*"
        SUITE_GLOB="ableton_live_suite_*.zip" SUITE_SKIP="ableton_live_suite_11.*"
        ;;
    11)
        LIVE_FULL=11.3.43 PUSH_VERSION=1.2.23
        LIVE_SHA256=e102adf579891cb5f2ad234862fa643c3e51a33c29e1c4c5197aa45bca466d76
        # Peak: recovery.img (about 5.4 GB used of its 6.4 GB) and CombinedImage.zip
        # (about 5.3 GB) side by side; the Suite part alone: its zip unpacked (4.6 GB)
        # and the extracted files (1.9 GB).
        NEEDED_GB=12 PROGRAM_DONE_GB=7 AFTER_GB=4
        PUSH3_BYTES=2122536512  # /opt/push3
        SUITE_BYTES=1899934494  # SUITE_PARTS
        # FFmpeg 4.2, ICU 66, OpenSSL 1.1, Theora, bzip2 under its .so.1 name
        COMPAT=(libavcodec.so.58.54.100 libavformat.so.58.29.100 libavutil.so.56.31.100 libswresample.so.3.5.100
                libicudata.so.66.1 libicui18n.so.66.1 libicuuc.so.66.1 libcrypto.so.1.1 libssl.so.1.1
                libtheoradec.so.1.1.4 libtheoraenc.so.1.1.2 libbz2.so.1.0.8)
        NSL=libnsl.so.2.0.0
        # tools/wix-suite-extract.py (Live 11's installer is a WiX bundle)
        SUITE_PARTS=("Resources/Core Library" "Resources/Builtin Lessons" "Resources/Builtin"
                     "Resources/Cursors" "Resources/Icons" "Program/Installation.cfg")
        PUSH_GLOB="Push3-RecoveryImage-1.*.zip" PUSH_SKIP=""
        SUITE_GLOB="ableton_live_suite_11.*.zip" SUITE_SKIP=""
        ;;
    esac
    FALLBACK=("$NSL" libxcb-icccm.so.4.0.0 libxcb-image.so.0.0.0 libxcb-util.so.1.0.0
              libxcb-keysyms.so.1.0.0 libxcb-render-util.so.0.0.0 libxcb-xinerama.so.0.0.0
              libxcb-xinput.so.0.1.0 libxcb-xkb.so.1.0.0)
}
export DEBUGFS_PAGER=__none__

# --- terminal output ------------------------------------------------------------------
# A numbered line per step, a progress bar under it while it runs, then the
# time it took. Without a terminal (a log file) only the lines are written.
STEPS=13
STEP=0
STEP_START=$SECONDS
if [ -t 1 ]; then
    TTY=1 BOLD=$'\e[1m' DIM=$'\e[2m' GREEN=$'\e[32m' RED=$'\e[31m' YELLOW=$'\e[33m' RESET=$'\e[0m'
else
    TTY=0 BOLD="" DIM="" GREEN="" RED="" YELLOW="" RESET=""
fi

clear_line() { [ $TTY = 0 ] || printf '\r\e[K'; }
step() {
    STEP=$((STEP + 1))
    STEP_START=$SECONDS
    printf '%s[%2d/%d]%s %s\n' "$BOLD" "$STEP" "$STEPS" "$RESET" "$1"
    [ -z "${LOG:-}" ] || printf '\n== %s [%d/%d] %s\n' "$(date '+%F %T')" "$STEP" "$STEPS" "$1" >> "$LOG"
}
ok() {
    local t=$((SECONDS - STEP_START))
    clear_line
    printf '        %s✓%s %s%s%dm%02ds%s\n' "$GREEN" "$RESET" "${1:+$1  }" "$DIM" $((t / 60)) $((t % 60)) "$RESET"
}
note() { clear_line; printf '        %s%s%s\n' "$YELLOW" "$*" "$RESET"; }
# Once the install folder is in use, die also shows what the tools said in
# this step (install.log), whether the disk is full, and that a new run
# continues from here.
die() {
    local said free
    DIED=1
    clear_line
    printf '\n%serror:%s %s\n' "$RED$BOLD" "$RESET" "$*" >&2
    [ -n "${LOG:-}" ] || exit 1
    said=$(awk '/^== / { said = ""; next } { said = said $0 "\n" } END { printf "%s", said }' "$LOG" 2>/dev/null | tail -n 6)
    [ -z "$said" ] || printf '       The tools said (all in %s):\n%s\n' "$LOG" "$(sed 's/^/         /' <<< "$said")" >&2
    printf 'error: %s\n' "$*" >> "$LOG" 2>/dev/null || true
    free=$(df -Pk "$PREFIX" 2>/dev/null | awk 'NR==2 {print $4 * 1024}')
    [ -z "$free" ] || [ "$free" -ge 1000000000 ] ||
        printf '       %s is full (%s free): make space there first.\n' "$PREFIX" "$(human "$free")" >&2
    [ "$STEP" -le 2 ] || [ "${UPDATING:-0}" = 1 ] || printf '       Run install.sh again to continue; what is finished stays.\n' >&2
    exit 1
}
human() { awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.1f GB", b / 1e9; else printf "%.0f MB", b / 1e6 }'; }
# bar DONE TOTAL: one line, redrawn in place.
bar() {
    [ $TTY = 1 ] || return 0
    local done=$1 total=$2 cols width fill pct
    [ "$total" -gt 0 ] || total=1
    [ "$done" -le "$total" ] || done=$total
    cols=$(tput cols 2>/dev/null || echo 80)
    width=$((cols - 38))
    [ $width -ge 10 ] || width=10
    [ $width -le 50 ] || width=50
    pct=$((done * 100 / total))
    fill=$((done * width / total))
    printf '\r\e[K        [%s%s] %3d%%  %s of %s' "$(printf '%*s' $fill '' | tr ' ' '#')" \
        "$(printf '%*s' $((width - fill)) '' | tr ' ' '.')" $pct "$(human "$done")" "$(human "$total")"
}
# watch PID PATH TOTAL: progress of a background command by the size of what it writes.
watch_size() {
    local pid=$1 path=$2 total=$3
    while kill -0 "$pid" 2>/dev/null; do
        bar "$(du -sb "$path" 2>/dev/null | cut -f1 || echo 0)" "$total"
        sleep 0.5
    done
    wait "$pid"
}
# ask "question" default(y|n): reads the terminal, takes the default without one.
ask() {
    local answer=""
    if (exec </dev/tty) 2>/dev/null; then read -r -p "        $1 " answer </dev/tty || echo; fi
    answer=${answer:-$2}
    [[ $answer == [yY]* ]]
}

deps_hint() {
    local id="" like=""
    if [ -r /etc/os-release ]; then
        id=$(. /etc/os-release && echo "${ID:-}")
        like=$(. /etc/os-release && echo "${ID_LIKE:-}")
    fi
    # Live 11's Suite installer is unpacked with cabextract and python3.
    local extra="" arch=""
    [ "${LIVE_MAJOR:-12}" != 11 ] || { extra=" cabextract python3" arch=" cabextract python"; }
    case " $id $like " in
        *" arch "*) echo "sudo pacman -S --needed unzip e2fsprogs$arch pipewire-alsa" ;;
        *" debian "* | *" ubuntu "*) echo "sudo apt install unzip e2fsprogs$extra pipewire-alsa" ;;
        *" fedora "*) echo "sudo dnf install unzip e2fsprogs$extra pipewire-alsa" ;;
        *" suse "* | *" opensuse "*) echo "sudo zypper install unzip e2fsprogs$extra pipewire-alsa" ;;
        *) echo "install: unzip, e2fsprogs (debugfs),${extra:+ cabextract, python3,} pipewire-alsa" ;;
    esac
}

# zip_member ZIP REGEX: the first member whose name matches (case-insensitive).
# The listing is read whole: grep -q or -m1 in a pipe stops reading early, and
# with pipefail the writer's SIGPIPE then counts as a failure.
zip_member() {
    local list
    list=$(unzip -Z1 "$1" 2>/dev/null) || return 1
    re="$2" awk 'tolower($0) ~ tolower(ENVIRON["re"]) { print; found = 1; exit } END { exit !found }' <<< "$list"
}

soname() { local rest=${1#*.so.}; echo "${1%%.so.*}.so.${rest%%.*}"; }

# Start and size in bytes of GPT partition N of the disk image whose first MiB is in FILE.
gpt_partition() {
    local file=$1 n=$2 table esize entry first last
    [ "$(dd if="$file" bs=1 skip=512 count=8 status=none)" = "EFI PART" ] || return 1
    table=$(od -An -t u8 -j 584 -N 8 "$file")
    esize=$(od -An -t u4 -j 596 -N 4 "$file")
    entry=$((table * 512 + (n - 1) * esize))
    first=$(od -An -t u8 -j $((entry + 32)) -N 8 "$file")
    last=$(od -An -t u8 -j $((entry + 40)) -N 8 "$file")
    echo $((first * 512)) $(((last - first + 1) * 512))
}

# extract_partition ZIP MEMBER N OUT: partition N of the disk image MEMBER in
# ZIP to the file OUT (zero blocks left as holes), streamed: the whole image
# is never on disk. The first dd only counts what passes, for the bar.
extract_partition() {
    local zip=$1 member=$2 n=$3 out=$4 range start size pid read
    (unzip -p "$zip" "$member" 2>/dev/null || true) | head -c 1048576 > gpt.bin
    range=$(gpt_partition gpt.bin "$n") || die "could not read the partition table of $member"
    read -r start size <<< "$range"
    rm -f gpt.bin
    : > dd.log
    ( (unzip -p "$zip" "$member" 2>>"$LOG" || true) | { dd bs=4M status=progress 2> dd.log || true; } |
        dd of="$out" bs=4M iflag=fullblock,skip_bytes,count_bytes skip="$start" count="$size" conv=sparse status=none ) &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        [ -e dd.log ] || die "the work folder $WORK was removed while installing"
        read=$(tr '\r' '\n' < dd.log | grep -o '^[0-9]*' | tail -1 || true)
        bar "${read:-0}" $((start + size))
        sleep 0.5
    done
    wait $pid || die "could not read $member"
    rm -f dd.log
    [ "$(stat -c %s "$out")" = "$size" ] || die "$member ends early: is $zip complete?"
}

# copy_lib NAME DIR: a library GTK desktops often lack, from this system, or
# the image's copy when the system has none.
copy_lib() {
    local name=$1 dir=$2 d
    [ ! -e "$dir/$name" ] || return 0
    for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib "$WORK/fallback"; do
        if [ -e "$d/$name" ]; then
            cp -L "$d/$name" "$dir/$name"
            return 0
        fi
    done
    die "$name is missing; install it or copy it to $dir"
}

# --- arguments and checks ---------------------------------------------------
ARGC=$#
while [ $# -gt 0 ]; do
    case $1 in
        --update) UPDATE=1; if [ $# -gt 1 ] && [[ $2 != -* ]]; then UPDATE_DIR=$2; shift; fi; shift ;;
        --version) LIVE_MAJOR=${2:?--version needs 12 or 11}; shift 2 ;;
        --prefix) PREFIX=${2:?--prefix needs a folder}; PREFIX_SET=1; shift 2 ;;
        --edition) EDITION=${2:?--edition needs 1 (Suite), 2 (Standard), 3 (Intro) or 4 (Lite)}; shift 2 ;;
        --portable) PORTABLE=1; shift ;;
        --cpu) CPU_CHOICE=${2:?--cpu needs auto, avx512 or avx2}; shift 2 ;;
        -h | --help) sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (usage: install.sh [--version 12|11] [--edition N] [--prefix DIR] [--portable] [--cpu auto|avx512|avx2] | install.sh --update [DIR])" ;;
    esac
done
case $CPU_CHOICE in auto | avx512 | avx2) ;; *) die "--cpu takes auto, avx512 or avx2, not $CPU_CHOICE" ;; esac
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" && pwd)
# zips PATTERN SKIP: the zips in this script's folder that match PATTERN but not
# SKIP (the other version's), any case.
zips() {
    local f
    shopt -s nullglob nocaseglob nocasematch
    for f in "$SCRIPT_DIR"/$1; do
        [ -n "$2" ] && [[ ${f##*/} == $2 ]] && continue
        printf '%s\n' "$f"
    done
    shopt -u nullglob nocaseglob nocasematch
}
# --- questions ----------------------------------------------------------------
# On a terminal the installer asks what the options did not give: which Live
# (--version), a normal or a portable installation (--portable), and the install
# folder (--prefix). Without a terminal: the version whose files are here (12
# when both or none), a normal installation, the default folder.
INTERACTIVE=0
if (exec </dev/tty) 2>/dev/null; then INTERACTIVE=1; fi
ASKED=0
# files_of 12|11: the names of that version's two zips here, or nothing when one is missing.
files_of() {
    local p s
    if [ "$1" = 12 ]; then
        p=$(zips "Push3-RecoveryImage-*.zip" "Push3-RecoveryImage-1.*")
        s=$(zips "ableton_live_suite_*.zip" "ableton_live_suite_11.*")
    else
        p=$(zips "Push3-RecoveryImage-1.*.zip" "")
        s=$(zips "ableton_live_suite_11.*.zip" "")
    fi
    p=${p%%$'\n'*} s=${s%%$'\n'*}
    if [ -n "$p" ] && [ -n "$s" ]; then echo "${p##*/}, ${s##*/}"; fi
}
# answer PROMPT: one line from the terminal into $answer; no answer (Ctrl+D) stops.
answer() {
    answer=""
    read -r -p "$1" answer </dev/tty || { echo; die "no answer; nothing was installed"; }
}
# own_entry NAME: whether a top-level entry of the install folder is the installer's.
own_entry() {
    case $1 in
        push3_opt | compat_lib | shim | tools | live-home | Options-desktop.txt | start-live.sh | install.log | uninstall.sh | uninstall.sh.part | \
            .install.lock | .install-program | .install-content | .install-work.* | .ableton-live-linux | .update-backup | .update-backup.part) return 0 ;;
    esac
    return 1
}
# prefix_problem DIR (absolute): why DIR cannot be the install folder, or nothing.
# The question checks what the checks further down would stop on.
prefix_problem() {
    local dir=$1 entry others=()
    case $dir in *[[:space:]\'\"\\\$\`]*) echo "The folder must not contain spaces or quotes."; return 0 ;; esac
    case $dir/ in
        // | /bin/* | /boot/* | /dev/* | /etc/* | /lib/* | /lib64/* | /proc/* | /run/* | /sbin/* | /sys/* | /usr/* | /var/*)
            echo "$dir is a system folder; choose a folder of its own."; return 0 ;;
    esac
    case $(realpath -m -- "$HOME")/ in
        "$dir"/*) echo "$dir is your home folder or contains it; choose a folder of its own, for example $dir/ableton-live-$LIVE_MAJOR."; return 0 ;;
    esac
    if [ -e "$dir" ] && [ ! -d "$dir" ]; then
        echo "$dir exists and is not a folder."; return 0
    fi
    [ -d "$dir" ] || return 0
    if [ -e "$dir/.ableton-live-linux" ] && ! grep -q "^Ableton Live $LIVE_MAJOR for Linux" "$dir/.ableton-live-linux"; then
        echo "$dir holds an installation of another Live version; choose another folder."; return 0
    fi
    # An earlier installation there may hold other files; installing again keeps them.
    [ ! -e "$dir/push3_opt" ] && [ ! -e "$dir/.install-program" ] || return 0
    while IFS= read -r -d '' entry; do
        own_entry "${entry##*/}" || others+=("${entry##*/}")
    done < <(find "$dir" -mindepth 1 -maxdepth 1 -print0 2>/dev/null)
    if [ ${#others[@]} -gt 0 ]; then
        echo "$dir is not empty (${others[*]:0:3}$([ ${#others[@]} -le 3 ] || echo ' ...')); the uninstaller deletes the folder's contents, so choose a new or empty folder, for example $dir/ableton-live-$LIVE_MAJOR."
    fi
}

# --- what install and update share ---------------------------------------------------
# check_mods: this project's files next to install.sh.
check_mods() {
    [ -f "$SCRIPT_DIR/shim/libmenufix.so" ] && [ -f "$SCRIPT_DIR/shim/libpathfix.so" ] && [ -f "$SCRIPT_DIR/shim/libpackmeta.so" ] &&
        [ -f "$SCRIPT_DIR/shim/libswupdate.so.0.1" ] && [ -f "$SCRIPT_DIR/tools/AbletonLive$LIVE_MAJOR-launcher" ] ||
        die "shim/ and tools/ must be next to install.sh (keep the folder as it came)"
}
# The project's version (VERSION next to install.sh), recorded in the install folder's marker.
PROJECT_VERSION=$(head -n1 "$(dirname "${BASH_SOURCE[0]:-.}")/VERSION" 2>/dev/null || true)
PROJECT_VERSION=${PROJECT_VERSION:-unknown}
# write_marker: the install folder's marker (uninstall.sh and the update check it), with the version.
write_marker() {
    rm -f "$PREFIX/.ableton-live-linux"
    printf 'Ableton Live %s for Linux install folder; uninstall.sh removes it\nversion %s\n' "$LIVE_MAJOR" "$PROJECT_VERSION" \
        > "$PREFIX/.ableton-live-linux"
}
# installed_version DIR: the version an installation recorded; empty before versions were recorded.
installed_version() { { sed -n 's/^version //p' "$1/.ableton-live-linux" 2>/dev/null || true; } | head -n1; }

# --- finding an installation -------------------------------------------------------
# install_state DIR: 0 a finished installation made by this installer (any release), 2 one
# that did not finish, 1 none. Every release writes uninstall.sh last; one whose uninstall.sh
# was lost still has the shim.
install_state() {
    [ -x "$1/push3_opt/push3/Live" ] || return 1
    [ ! -e "$1/uninstall.sh" ] || return 0
    [ ! -e "$1/.install-program" ] || return 2
    [ -f "$1/shim/libmenufix.so" ] || return 1
    return 0
}
# live_major_of DIR: 12 or 11, from the marker, else the launcher, else Live itself.
live_major_of() {
    local m
    m=$({ sed -n 's/^Ableton Live \([0-9]*\) for Linux.*/\1/p' "$1/.ableton-live-linux" 2>/dev/null || true; } | head -n1)
    if [ -z "$m" ]; then
        if [ -e "$1/push3_opt/push3/AbletonLive11" ]; then m=11
        elif [ -e "$1/push3_opt/push3/AbletonLive12" ]; then m=12
        else
            case $(sha256sum "$1/push3_opt/push3/Live" 2>/dev/null | cut -d' ' -f1) in
                8f9052719b35d6add174c2fe9a61897c7115717bc4d3d6623697881976251dc0) m=12 ;;
                e102adf579891cb5f2ad234862fa643c3e51a33c29e1c4c5197aa45bca466d76) m=11 ;;
            esac
        fi
    fi
    case $m in 12 | 11) echo "$m" ;; esac
}
# describe DIR: "Live 12, portable, version X" for the lists.
describe() {
    local kind=normal v
    [ ! -d "$1/live-home" ] || grep -qsF "$1/push3_opt/" "$DATA_HOME"/applications/ableton-live-1[12].desktop || kind=portable
    v=$(installed_version "$1")
    printf 'Live %s, %s, %s' "$(live_major_of "$1")" "$kind" "$([ -n "$v" ] && echo "version $v" || echo "an earlier version")"
}
# find_installs: "STATE<tab>DIR" for each installation found: the default folders, then the
# home folder and mounted drives (a few levels deep; inside an installation, caches and
# trash not). STATE as install_state.
find_installs() {
    local d roots=()
    for d in "$HOME" "/run/media/$(id -un)" /media /mnt; do [ -d "$d" ] && roots+=("$d"); done
    {
        for d in "$DATA_HOME/ableton-live-12" "$DATA_HOME/ableton-live-11" "$SCRIPT_DIR/AbletonLive12-portable" \
            "$SCRIPT_DIR/AbletonLive11-portable"; do
            [ ! -d "$d/push3_opt" ] || printf '%s\n' "$d"
        done
        timeout 60 find "${roots[@]}" -maxdepth 6 \( -name live-home -o -name .cache -o -name .git -o -name node_modules \
            -o -name Trash -o -name '.Trash-*' -o -name .snapshots -o -name proc \) -prune \
            -o -type d -name push3_opt -prune -printf '%h\n' 2>/dev/null || true
    } | while IFS= read -r d; do
        d=$(realpath -e -- "$d" 2>/dev/null) || continue
        s=0
        install_state "$d" || s=$?
        [ $s = 1 ] || printf '%s\t%s\n' "$s" "$d"
    done | sort -u -t $'\t' -k2,2
}
# folder_answer TEXT: a typed or dragged-in folder (quotes and ~ handled), absolute.
folder_answer() {
    local d=$1
    d=${d#"${d%%[![:space:]]*}"} d=${d%"${d##*[![:space:]]}"}
    case $d in \'*\') d=${d:1:-1} ;; \"*\") d=${d:1:-1} ;; file://*) d=${d#file://} ;; esac
    case $d in "~") d=$HOME ;; "~/"*) d=$HOME/${d#"~/"} ;; esac
    realpath -m -- "$d"
}
# ask_install_folder: asks until a folder with an installation is given; sets CHOSEN.
# No answer: nothing changes.
ask_install_folder() {
    local d s
    while :; do
        answer "Folder of the installation (type it or drag it here; Enter to stop): "
        [ -n "$answer" ] || { printf 'Nothing was changed.\n'; exit 0; }
        d=$(folder_answer "$answer")
        s=0
        install_state "$d" || s=$?
        case $s in
            0) CHOSEN=$d; return 0 ;;
            2) printf '  %s%s holds an installation that did not finish: run install.sh without --update to continue it.%s\n' "$YELLOW" "$d" "$RESET" ;;
            *) printf '  %sNo installation of Ableton Live made with this installer in %s (it has push3_opt/ and shim/).%s\n' "$YELLOW" "$d" "$RESET" ;;
        esac
    done
}
# choose_install NEW: lists the installations found and asks which one to update. NEW=1 also
# offers a new installation (CHOSEN empty). Sets CHOSEN.
choose_install() {
    local new=$1 list=() done_list=() line state dir i zips
    CHOSEN=""
    printf '\n%sLooking for installations of Ableton Live...%s\n' "$DIM" "$RESET"
    mapfile -t list < <(find_installs)
    for line in "${list[@]}"; do
        state=${line%%$'\t'*} dir=${line#*$'\t'}
        if [ "$state" = 0 ]; then done_list+=("$dir")
        else printf '  %sAn installation that did not finish is in %s; install.sh with --prefix %s continues it.%s\n' "$YELLOW" "$dir" "$dir" "$RESET"; fi
    done
    zips="$(files_of 12)$(files_of 11)"
    if [ ${#done_list[@]} -gt 0 ]; then
        printf '\n%sUpdate an installation, or install Live?%s\n' "$BOLD" "$RESET"
        for i in "${!done_list[@]}"; do
            printf '  %s%d%s  Update %s%s\n        %s\n' "$BOLD" $((i + 1)) "$RESET" "$(describe "${done_list[i]}")" \
                "$([ $i != 0 ] || echo "  (default)")" "${done_list[i]/#"$HOME"/\~}"
        done
        [ "$new" = 0 ] || printf '  %s%d%s  Install Live (a new installation)%s\n' "$BOLD" $((${#done_list[@]} + 1)) "$RESET" \
            "$([ -n "$zips" ] || echo ': needs the Push 3 image and the Suite installer next to install.sh')"
        printf '  Or type the folder of another installation.\n'
        while :; do
            answer "Type a number or a folder, or press Enter for 1: "
            answer=${answer:-1}
            if [[ $answer =~ ^[0-9]+$ ]]; then
                if [ "$answer" -ge 1 ] && [ "$answer" -le ${#done_list[@]} ]; then CHOSEN=${done_list[answer - 1]}; return 0; fi
                if [ "$new" = 1 ] && [ "$answer" = $((${#done_list[@]} + 1)) ]; then
                    [ -n "$zips" ] && return 0
                    printf '  %sThe Push 3 image and the Suite installer are not in %s.%s\n' "$YELLOW" "$SCRIPT_DIR" "$RESET"
                    continue
                fi
                printf '  %sPlease type a number from the list.%s\n' "$YELLOW" "$RESET"
                continue
            fi
            dir=$(folder_answer "$answer")
            state=0
            install_state "$dir" || state=$?
            [ $state != 0 ] || { CHOSEN=$dir; return 0; }
            printf '  %sNo finished installation made with this installer in %s.%s\n' "$YELLOW" "$dir" "$RESET"
        done
    fi
    if [ "$new" = 1 ] && [ -n "$zips" ]; then
        printf '\n%sNo installation of Ableton Live was found.%s\n' "$BOLD" "$RESET"
        while :; do
            answer "Install Live [I], or update an installation in another folder [u]? "
            case ${answer:-i} in
                [iI]*) return 0 ;;
                [uU]*) ask_install_folder; return 0 ;;
                *) printf '  %sPlease type i or u.%s\n' "$YELLOW" "$RESET" ;;
            esac
        done
    fi
    if [ "$new" = 1 ]; then
        printf '\n%sNo installation of Ableton Live was found, and the Push 3 image and the Suite installer%s\n' "$BOLD" "$RESET"
        printf '%sare not next to install.sh, so it can only update an installation in another folder.%s\n' "$BOLD" "$RESET"
    else
        printf '\n%sNo installation of Ableton Live was found.%s\n' "$BOLD" "$RESET"
    fi
    ask_install_folder
}

# --- update ---------------------------------------------------------------------------
# install.sh --update [DIR]: replaces this project's files in an installation (shim, launcher,
# tools, fallback libraries, start script, menu entry, uninstaller) and keeps Live, the
# content from the Suite installer, the edition, settings, licence and libraries. Needs no
# Push 3 image and no Suite installer. Works on installations from every release: what an
# older one did not record (the marker, uninstall.sh's settings) is found and written.
# The files it replaces are kept in .update-backup/ (the last update's only) and put back
# when the update stops.
UPDATING=0 UPDATE_DONE=0 BACKUP_READY=0
# The files the update replaces: "in" relative to the install folder, "out" absolute.
backup_list() {
    local rel
    for rel in shim tools Options-desktop.txt start-live.sh uninstall.sh .ableton-live-linux compat_lib/libswupdate.so.0.1 \
        compat_lib/openssl3 compat_lib/libxml2 "push3_opt/push3/AbletonLive$LIVE_MAJOR" push3_opt/push3/lib/xkbcommon; do
        printf 'in\t%s\n' "$rel"
    done
    [ -z "$DESKTOP" ] || printf 'out\t%s\nout\t%s\n' "$DESKTOP" "$MIME_XML"
}
make_backup() {
    local where path src
    rm -rf "$BACKUP" "$BACKUP.part"
    mkdir -p "$BACKUP.part"
    while IFS=$'\t' read -r where path; do
        if [ $where = in ]; then src=$PREFIX/$path; else src=$path; fi
        if [ -e "$src" ] || [ -L "$src" ]; then
            mkdir -p "$BACKUP.part/$where/$(dirname "$path")"
            cp -a "$src" "$BACKUP.part/$where/$path" || return 1
            printf 'present\t%s\t%s\n' "$where" "$path"
        else
            printf 'absent\t%s\t%s\n' "$where" "$path"
        fi
    done < <(backup_list) > "$BACKUP.part/manifest"
    mv "$BACKUP.part" "$BACKUP"
}
restore_backup() {
    local state where path dst
    while IFS=$'\t' read -r state where path; do
        if [ $where = in ]; then dst=$PREFIX/$path; else dst=$path; fi
        rm -rf "$dst"
        [ $state = absent ] || { mkdir -p "$(dirname "$dst")"; cp -a "$BACKUP/$where/$path" "$dst"; }
    done < "$BACKUP/manifest"
}
update_exit() {
    local status=$1
    [ $TTY = 0 ] || printf '\e[?25h'
    if [ "$status" != 0 ] && [ "${DIED:-0}" != 1 ] && [ $UPDATING = 1 ] && [ $UPDATE_DONE = 0 ]; then
        printf '\n%serror:%s the update stopped at a command that failed (see above; also %s)\n' "$RED$BOLD" "$RESET" "${LOG:-install.log}" >&2
    fi
    if [ $UPDATING = 1 ] && [ $UPDATE_DONE = 0 ] && [ $BACKUP_READY = 1 ]; then
        if restore_backup 2>/dev/null; then
            printf '%sThe update stopped; the installation is as it was before.%s\n' "$YELLOW" "$RESET" >&2
        else
            printf '%sThe update stopped, and putting the old files back failed: they are in %s.%s\n' "$RED" "$BACKUP" "$RESET" >&2
        fi
    fi
    exit "$status"
}
# cpu_lacks_avx512: whether this CPU lacks the AVX-512 subset Live is built for.
cpu_lacks_avx512() {
    local flags flag
    flags=" $(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null | cut -d: -f2) "
    for flag in avx512f avx512cd avx512bw avx512dq avx512vl avx512vbmi avx512_vbmi2 avx512_vnni avx512_vpopcntdq gfni; do
        [[ $flags == *" $flag "* ]] || return 0
    done
    return 1
}
update_main() {
    local dir=$1 list=() line s old_version old_prefix moved="" music lines
    if [ -z "$dir" ]; then
        if [ $INTERACTIVE = 1 ]; then
            choose_install 0
            dir=$CHOSEN
        else
            mapfile -t list < <(find_installs | awk -F'\t' '$1 == 0 { print $2 }')
            [ ${#list[@]} -gt 0 ] || die "no installation of Ableton Live found; give its folder: install.sh --update FOLDER"
            [ ${#list[@]} -eq 1 ] || die "several installations found; give the folder of one: install.sh --update FOLDER
$(printf '         %s\n' "${list[@]}")"
            dir=${list[0]}
        fi
    fi
    UPDATING=1
    trap 'update_exit $?' EXIT
    trap 'exit 130' INT TERM
    trap 'exit 129' HUP
    [ "$(id -u)" != 0 ] || die "run this as your normal user"
    dir=$(folder_answer "$dir")
    s=0
    install_state "$dir" || s=$?
    [ $s != 2 ] || die "$dir holds an installation that did not finish; run install.sh --prefix $dir to continue it"
    [ $s = 0 ] || die "no installation of Ableton Live made with this installer in $dir"
    PREFIX=$dir APP=$dir/push3_opt/push3 MODS=$SCRIPT_DIR
    s=$(live_major_of "$PREFIX")
    [ -n "$s" ] || die "cannot tell which Live is installed in $PREFIX"
    [ -z "$LIVE_MAJOR" ] || [ "$LIVE_MAJOR" = "$s" ] || die "$PREFIX holds Live $s, not Live $LIVE_MAJOR"
    LIVE_MAJOR=$s
    set_version
    check_mods
    ! live_running || die "Live is running from $PREFIX; quit it first"
    [ -w "$PREFIX" ] || die "$PREFIX is not writable"
    exec 9>"$PREFIX/.install.lock"
    if command -v flock >/dev/null && ! flock -n 9; then die "an installation or update in $PREFIX is still running"; fi
    LOG="$PREFIX/install.log"
    printf '\n== %s install.sh --update from %s in %s\n' "$(date '+%F %T')" "$SCRIPT_DIR" "$PREFIX" >> "$LOG" ||
        die "could not write to $PREFIX"
    old_version=$(installed_version "$PREFIX")

    # What the installation recorded in its uninstall.sh (its values are written with %q).
    local OLD_PREFIX="" OLD_DESKTOP="" OLD_MIME_XML="" OLD_LOADER_LINK="" OLD_CORE="" OLD_UDEV_RULE="" HAD_UNINSTALLER=0
    if [ -f "$PREFIX/uninstall.sh" ]; then
        HAD_UNINSTALLER=1
        while IFS= read -r line; do
            case $line in
                PREFIX=* | DESKTOP=* | MIME_XML=* | LOADER_LINK=* | CORE=* | UDEV_RULE=*) eval "OLD_$line" ;;
            esac
        done < <(head -n 40 "$PREFIX/uninstall.sh")
    fi
    # A folder moved since: the Core Library inside it moved with it.
    if [ -n "$OLD_PREFIX" ] && [ "$OLD_PREFIX" != "$PREFIX" ]; then
        moved=$OLD_PREFIX
        case $OLD_CORE in "$OLD_PREFIX"/*) OLD_CORE=$PREFIX${OLD_CORE#"$OLD_PREFIX"} ;; esac
        [ ! -f "$OLD_DESKTOP" ] || grep -qF "$PREFIX/push3_opt/" "$OLD_DESKTOP" || OLD_DESKTOP="" OLD_MIME_XML=""
    fi
    # Normal or portable: a normal installation has a menu entry that starts it.
    if [ -n "$OLD_DESKTOP" ]; then PORTABLE=0
    elif [ $HAD_UNINSTALLER = 1 ] && [ -n "$OLD_PREFIX" ] && [ -d "$PREFIX/live-home" ]; then PORTABLE=1
    elif grep -qsF "$PREFIX/push3_opt/" "$DATA_HOME/applications/ableton-live-$LIVE_MAJOR.desktop"; then PORTABLE=0
    elif [ -d "$PREFIX/live-home" ]; then PORTABLE=1
    else PORTABLE=0; fi
    # The Core Library: as recorded, else where the installer puts it.
    CORE=$OLD_CORE
    if [ -z "$CORE" ] || [ ! -d "$CORE" ]; then
        if [ -d "$PREFIX/live-home" ]; then music=$PREFIX/live-home/Music
        else
            music=$({ sed -n 's/^XDG_MUSIC_DIR=//p' "${XDG_CONFIG_HOME:-$HOME/.config}/user-dirs.dirs" 2>/dev/null || true; } | head -n1 | tr -d '"')
            music=${music/\$HOME/$HOME}
            music=${music:-$HOME/Music}
        fi
        [ ! -d "$music/Ableton/Live $LIVE_MAJOR Core Library" ] || CORE="$music/Ableton/Live $LIVE_MAJOR Core Library"
    fi
    LOADER_LINK=${OLD_LOADER_LINK:-0}
    UDEV_RULE=${OLD_UDEV_RULE:-0}
    [ -n "$OLD_UDEV_RULE" ] || ! grep -qs "Ableton Live $LIVE_MAJOR, install.sh" /etc/udev/rules.d/70-ableton-push.rules || UDEV_RULE=1
    # The CPU version it has: the emulator is there for the AVX2 version (portable: both).
    if [ $CPU_CHOICE = auto ]; then
        if [ -f "$PREFIX/shim/libavx512emu.so" ] || cpu_lacks_avx512; then
            if [ $PORTABLE = 1 ]; then CPU_CHOICE=both; else CPU_CHOICE=avx2; fi
        else
            CPU_CHOICE=avx512
        fi
    fi
    [ $CPU_CHOICE = avx512 ] || [ -f "$MODS/shim/libavx512emu.so" ] ||
        die "the AVX2 version needs shim/libavx512emu.so next to install.sh (keep the folder as it came)"
    DESKTOP="" MIME_XML=""
    [ $PORTABLE = 1 ] || { DESKTOP="$DATA_HOME/applications/ableton-live-$LIVE_MAJOR.desktop"; MIME_XML="$DATA_HOME/mime/packages/ableton-live-$LIVE_MAJOR.xml"; }
    BACKUP=$PREFIX/.update-backup

    STEPS=5
    printf '\n%sAbleton Live %s for Linux: update%s\n' "$BOLD" "$LIVE_MAJOR" "$RESET"
    printf '  Installation  %s%s\n  From          %s\n  To            version %s\n\n' "$PREFIX" "$([ $PORTABLE = 0 ] || echo ' (portable)')" \
        "$([ -n "$old_version" ] && echo "version $old_version" || echo "an earlier version")" "$PROJECT_VERSION"
    if [ $INTERACTIVE = 1 ]; then
        answer "Update it? Live, its content, edition, settings, licence and libraries stay. [Y/n] "
        [[ ${answer:-y} == [yY]* ]] || { UPDATING=0; printf 'Nothing was changed.\n'; exit 0; }
        printf '\n'
    fi
    [ $TTY = 0 ] || printf '\e[?25l'

    step "Checking the installation"
    [ "$(sha256sum "$APP/Live" | cut -d' ' -f1)" = "$LIVE_SHA256" ] ||
        note "This is not Live $LIVE_FULL (Push 3 $PUSH_VERSION), which the shim was made for; fixes that do not match stay off."
    [ -z "$moved" ] || note "The folder moved from $moved since it was installed; the new uninstall.sh works here."
    [ $HAD_UNINSTALLER = 1 ] || note "It had no uninstall.sh; a new one is written."
    [ -n "$CORE" ] || note "Its Core Library was not found; the uninstaller leaves it."
    [ -e "$APP/Installation.cfg" ] || note "push3_opt/push3/Installation.cfg (the edition) is missing; Live starts as Suite."
    lines=$(df -Pk "$PREFIX" | awk 'NR==2 {print $4}')
    [ "$lines" -ge 524288 ] || die "$PREFIX has $(human $((lines * 1024))) free; the update needs about 500 MB"
    ok "Live $LIVE_FULL, $([ $PORTABLE = 0 ] && echo normal || echo portable), $(case $CPU_CHOICE in avx512) echo 'AVX-512 version' ;; avx2) echo 'AVX2 version' ;; *) echo 'both CPU versions' ;; esac)"

    step "Keeping the files it replaces"
    make_backup || die "could not copy the files the update replaces to $BACKUP"
    BACKUP_READY=1
    ok "${BACKUP/#"$HOME"/\~}"

    step "Installing the new shim and launcher"
    install_mods
    ok

    step "Start script and menu entry"
    write_start_script
    if [ $PORTABLE = 1 ]; then
        ok "portable: no menu entry; start Live with $PREFIX/start-live.sh"
    else
        write_menu_entry
        ok ".als and .alp files, ableton:// links"
    fi

    step "Writing the uninstaller"
    write_uninstaller
    write_marker
    rm -f "$PREFIX/.install-program" "$PREFIX/.install-content"
    UPDATE_DONE=1
    ok "$PREFIX/uninstall.sh"
    [ $TTY = 0 ] || printf '\e[?25h'

    printf '\n%s%sDone.%s Ableton Live %s is updated to version %s.\n' "$GREEN" "$BOLD" "$RESET" "$LIVE_MAJOR" "$PROJECT_VERSION"
    printf 'The replaced files are in %s (the next update replaces them).\n' "${BACKUP/#"$HOME"/\~}"
    # The hardware code fix (shim/licensing.cpp): on these roots Live's code changes once more.
    if [ -z "$old_version" ]; then
        case $(findmnt -no FSTYPE / 2>/dev/null || true) in
            btrfs | overlay | composefs | tmpfs | zfs)
                note "Live may ask to authorize once more: from now on its hardware code stays the same on this system (README, Troubleshooting)." ;;
        esac
    fi
}

# live_running: whether a Live from this install folder runs.
live_running() {
    local p exe
    for p in /proc/[0-9]*; do
        exe=$(readlink "$p/exe" 2>/dev/null) || continue
        case $exe in "$PREFIX"/push3_opt/*) return 0 ;; esac
    done
    return 1
}

# install_mods: this project's files into the installation (install and update).
install_mods() {
    # Afresh: nothing of an older version stays (the update keeps them in .update-backup/).
    rm -rf "$PREFIX/shim" "$PREFIX/tools"
    mkdir -p "$PREFIX/shim" "$PREFIX/tools"
    cp "$MODS/shim/libmenufix.so" "$PREFIX/shim/"
    # The launcher preloads it when the install path has a hidden folder (the default
    # ~/.local/share/...): otherwise Live does not index its built-in devices.
    cp "$MODS/shim/libpathfix.so" "$PREFIX/shim/"
    # Pack metadata with Windows separators, fixed in memory for Live and its file indexer
    # (shim/packmeta.cpp); the launcher preloads it.
    cp "$MODS/shim/libpackmeta.so" "$PREFIX/shim/"
    # The Push 3 companion (Push3), which Live starts when Push hardware is connected, links
    # the Push OS update library; the image has it only in the device's system partition and
    # it waits forever for the device's update service. A stand-in that reports "no update
    # service" (shim/swupdate-stub.c), where Live's programs find their libraries.
    mkdir -p "$PREFIX/compat_lib"
    rm -f "$PREFIX/compat_lib/libswupdate.so.0.1"
    cp "$MODS/shim/libswupdate.so.0.1" "$PREFIX/compat_lib/"
    rm -f "$PREFIX/shim/libavx512emu.so"
    [ $CPU_CHOICE = avx512 ] || cp "$MODS/shim/libavx512emu.so" "$PREFIX/shim/"
    rm -f "$PREFIX/Options-desktop.txt"
    cp "$MODS/Options-desktop.txt" "$PREFIX/"
    cp -r "$MODS/tools/." "$PREFIX/tools/"
    # Only this version's launcher and Suite extractor: innoextract for Live 12's
    # installer (Inno Setup), tools/wix-suite-extract.py for Live 11's (WiX).
    if [ $LIVE_MAJOR = 12 ]; then
        rm -f "$PREFIX/tools/AbletonLive11-launcher" "$PREFIX/tools/wix-suite-extract.py"
        rm -rf "$PREFIX/tools/bundled/libxml2-icu66"
        extractor=$PREFIX/tools/bundled/innoextract
    else
        rm -f "$PREFIX/tools/AbletonLive12-launcher" "$PREFIX/tools/bundled/"{innoextract,LICENSE.innoextract} \
            "$PREFIX/tools/innoextract-compat.cpp"
        rm -rf "$PREFIX/tools/bundled/openssl3" "$PREFIX/tools/bundled/libxml2"
        extractor=$PREFIX/tools/wix-suite-extract.py
    fi
    # Executable here even when the folder came on a USB stick or a Windows file system.
    chmod +x "$PREFIX/tools/"*.sh "$PREFIX/tools/AbletonLive$LIVE_MAJOR-launcher" "$PREFIX/tools/make-assets" "$extractor"
    # Files in push3_opt are removed first: written into, a file hard-linked to another copy would change there too.
    rm -f "$APP/AbletonLive$LIVE_MAJOR"
    cp "$PREFIX/tools/AbletonLive$LIVE_MAJOR-launcher" "$APP/AbletonLive$LIVE_MAJOR"
    rm -rf "$APP/lib/xkbcommon"
    mkdir -p "$APP/lib/xkbcommon"
    cp "$PREFIX/tools/bundled/"lib*.so.* "$APP/lib/xkbcommon/"
    # OpenSSL 3: Live 12 lists libssl.so.3 and libcrypto.so.3 but calls nothing in them (it has
    # OpenSSL 1.1.1 built in), so it starts only where they exist; Fedora 45 has OpenSSL 4 only.
    # Debian 12's security-maintained build (tools/bundled/README) goes to compat_lib/openssl3,
    # which the launcher uses only when the system has no OpenSSL 3 (plug-ins may call it).
    # libxml2.so.2 the same way, for Max's maxxslt: distributions moved to libxml2.so.16.
    # Live 12: Ubuntu 22.04's maintained build (ICU 70 from compat_lib). Live 11 (no OpenSSL 3
    # needed: it links the image's OpenSSL 1.1): Ubuntu 20.04's last public build (ICU 66).
    rm -rf "$PREFIX/compat_lib/openssl3" "$PREFIX/compat_lib/libxml2"
    mkdir -p "$PREFIX/compat_lib/libxml2"
    if [ $LIVE_MAJOR = 12 ]; then
        mkdir -p "$PREFIX/compat_lib/openssl3"
        cp "$PREFIX/tools/bundled/openssl3/"lib*.so.3 "$PREFIX/compat_lib/openssl3/"
        cp "$PREFIX/tools/bundled/libxml2/libxml2.so.2" "$PREFIX/compat_lib/libxml2/"
    else
        cp "$PREFIX/tools/bundled/libxml2-icu66/libxml2.so.2" "$PREFIX/compat_lib/libxml2/"
    fi
}

# write_start_script: start-live.sh, which starts this Live from wherever the folder is.
write_start_script() {
    # start-live.sh: starts this Live from wherever the folder is now.
    rm -f "$PREFIX/start-live.sh"
    printf '#!/bin/sh\n# Starts Ableton Live from this folder (push3_opt/push3/AbletonLive%s also does).\nexec "$(dirname "$(readlink -f "$0")")/push3_opt/push3/AbletonLive%s" "$@"\n' \
        "$LIVE_MAJOR" "$LIVE_MAJOR" > "$PREFIX/start-live.sh"
    chmod +x "$PREFIX/start-live.sh"
}

# write_menu_entry: the menu entry and the file types (a normal installation); sets DESKTOP and MIME_XML.
write_menu_entry() {
    # The window's WM_CLASS instance (shim/window.cpp): Live 11 has its own, so the
    # desktop does not group its windows under Live 12's entry and icon.
    WM_CLASS=ableton-live
    [ $LIVE_MAJOR = 12 ] || WM_CLASS=ableton-live-$LIVE_MAJOR
    APPS="$DATA_HOME/applications"
    DESKTOP="$APPS/ableton-live-$LIVE_MAJOR.desktop"
    MIME_XML="$DATA_HOME/mime/packages/ableton-live-$LIVE_MAJOR.xml"
    mkdir -p "$APPS" "$(dirname "$MIME_XML")"
    cat > "$MIME_XML" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-ableton-live-set">
    <comment>Ableton Live Set</comment>
    <glob pattern="*.als"/>
  </mime-type>
  <mime-type type="application/x-ableton-live-pack">
    <comment>Ableton Live Pack</comment>
    <glob pattern="*.alp"/>
  </mime-type>
  <mime-type type="application/x-ableton-authorization">
    <comment>Ableton Authorization File</comment>
    <glob pattern="*.auz"/>
  </mime-type>
</mime-info>
EOF
    cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Ableton Live $LIVE_MAJOR
Comment=Music production software
Exec="$APP/AbletonLive$LIVE_MAJOR" %U
Path=$APP
Icon=$APP/products/live/Live/AppLive/Linux/Icons/live_256.png
Terminal=false
Categories=Audio;AudioVideo;
StartupWMClass=$WM_CLASS
MimeType=application/x-ableton-live-set;application/x-ableton-live-pack;application/x-ableton-authorization;x-scheme-handler/ableton;
EOF
    command -v update-mime-database >/dev/null && update-mime-database "$DATA_HOME/mime" >/dev/null 2>&1 || true
    command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" >/dev/null 2>&1 || true
    # ableton:// links (authorization in the browser) go to this Live. Live's own
    # registration points at the unmodified Live binary, which cannot start alone.
    # The launcher checks this at every start and repairs it (tools/linkhandler.h);
    # a portable installation gets its handler from there only.
    command -v xdg-mime >/dev/null && xdg-mime default "ableton-live-$LIVE_MAJOR.desktop" x-scheme-handler/ableton >/dev/null 2>&1 || true
}

# write_uninstaller: uninstall.sh, with what this installation put outside its folder.
write_uninstaller() {
{
    echo '#!/usr/bin/env bash'
    echo "# Removes Ableton Live $LIVE_MAJOR, installed by install.sh on $(date +%F): the program,"
    echo '# the Core Library the installer copied, the menu entry and its file types.'
    echo '# Your User Library, recordings and projects in <Music>/Ableton always stay.'
    echo "# Asks whether to delete Live's settings and licence too, and whether to"
    echo '# remove the loader link if the installer made it.'
    echo '#'
    echo '# usage: uninstall.sh [--purge]   (--purge: delete the settings and licence without asking)'
    echo 'set -euo pipefail'
    echo '# What install.sh installed outside the install folder.'
    printf 'LIVE_MAJOR=%q\nPREFIX=%q\nDESKTOP=%q\nMIME_XML=%q\nDATA_HOME=%q\nLOADER_LINK=%q\nCORE=%q\nUDEV_RULE=%q\n' \
        "$LIVE_MAJOR" "$PREFIX" "$DESKTOP" "$MIME_XML" "$DATA_HOME" "$LOADER_LINK" "$CORE" "$UDEV_RULE"
    echo "# The install folder's entries this installer makes; uninstall.sh deletes only these."
    declare -f own_entry
    cat <<'EOF'

# Started from a file manager there is no terminal to show the progress in:
# run again inside one, and keep its window open at the end.
if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${LIVE_INSTALLER_TERMINAL:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    self=$(readlink -f "$0")
    run=(env LIVE_INSTALLER_TERMINAL=1 bash -c 'bash "$0" "$@"; status=$?; echo; read -r -p "Press Enter to close this window. " _ </dev/tty; exit $status' "$self" "$@")
    case "${XDG_CURRENT_DESKTOP:-}" in
        *KDE*) order=(konsole) ;;
        *GNOME* | *Unity* | *Pantheon*) order=(kgx ptyxis gnome-terminal) ;;
        *Cinnamon* | *X-Cinnamon*) order=(gnome-terminal) ;;
        *XFCE*) order=(xfce4-terminal) ;;
        *MATE*) order=(mate-terminal) ;;
        *LXQt*) order=(qterminal) ;;
        *) order=() ;;
    esac
    order+=(x-terminal-emulator konsole kgx ptyxis gnome-terminal xfce4-terminal mate-terminal qterminal
            tilix alacritty kitty foot wezterm xterm)
    for term in "${order[@]}"; do
        command -v "$term" >/dev/null || continue
        case $term in
            kgx | ptyxis | gnome-terminal) exec "$term" -- "${run[@]}" ;;
            xfce4-terminal | mate-terminal) exec "$term" -x "${run[@]}" ;;
            kitty | foot) exec "$term" "${run[@]}" ;;
            wezterm) exec "$term" start -- "${run[@]}" ;;
            *) exec "$term" -e "${run[@]}" ;;
        esac
    done
    message="No terminal program found. Open a terminal in $(dirname "$self") and run ./uninstall.sh"
    command -v kdialog >/dev/null && exec kdialog --error "$message"
    command -v zenity >/dev/null && exec zenity --error --text="$message"
    command -v notify-send >/dev/null && exec notify-send "Ableton Live uninstaller" "$message"
    exit 1
fi

CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"

# --- terminal output ------------------------------------------------------------------
# The same as install.sh: a numbered line per step, a progress bar under it
# while it runs, then the time it took.
STEPS=5
STEP=0
STEP_START=$SECONDS
if [ -t 1 ]; then
    TTY=1 BOLD=$'\e[1m' DIM=$'\e[2m' GREEN=$'\e[32m' RED=$'\e[31m' YELLOW=$'\e[33m' RESET=$'\e[0m'
else
    TTY=0 BOLD="" DIM="" GREEN="" RED="" YELLOW="" RESET=""
fi

clear_line() { [ $TTY = 0 ] || printf '\r\e[K'; }
step() {
    STEP=$((STEP + 1))
    STEP_START=$SECONDS
    printf '%s[%2d/%d]%s %s\n' "$BOLD" "$STEP" "$STEPS" "$RESET" "$1"
}
ok() {
    local t=$((SECONDS - STEP_START))
    clear_line
    printf '        %s✓%s %s%s%dm%02ds%s\n' "$GREEN" "$RESET" "${1:+$1  }" "$DIM" $((t / 60)) $((t % 60)) "$RESET"
}
note() { clear_line; printf '        %s%s%s\n' "$YELLOW" "$*" "$RESET"; }
die() {
    clear_line
    printf '\n%serror:%s %s\n' "$RED$BOLD" "$RESET" "$*" >&2
    exit 1
}
human() { awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.1f GB", b / 1e9; else printf "%.0f MB", b / 1e6 }'; }
# bar DONE TOTAL: one line, redrawn in place.
bar() {
    [ $TTY = 1 ] || return 0
    local done=$1 total=$2 cols width fill pct
    [ "$total" -gt 0 ] || total=1
    [ "$done" -le "$total" ] || done=$total
    [ "$done" -ge 0 ] || done=0
    cols=$(tput cols 2>/dev/null || echo 80)
    width=$((cols - 38))
    [ $width -ge 10 ] || width=10
    [ $width -le 50 ] || width=50
    pct=$((done * 100 / total))
    fill=$((done * width / total))
    printf '\r\e[K        [%s%s] %3d%%  %s of %s' "$(printf '%*s' $fill '' | tr ' ' '#')" \
        "$(printf '%*s' $((width - fill)) '' | tr ' ' '.')" $pct "$(human "$done")" "$(human "$total")"
}
# ask "question" default(y|n): reads the terminal, takes the default without one.
ask() {
    local answer=""
    if (exec </dev/tty) 2>/dev/null; then read -r -p "        $1 " answer </dev/tty || echo; fi
    answer=${answer:-$2}
    [[ $answer == [yY]* ]]
}
size_of() { { du -sbc -- "$@" 2>/dev/null || true; } | tail -1 | cut -f1; }
# remove PATH...: deletes in the background, with a bar of how much is gone.
remove() {
    local total pid
    [ $# -gt 0 ] || return 0
    total=$(size_of "$@")
    rm -rf -- "$@" &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        bar $((total - $(size_of "$@"))) "$total"
        sleep 0.5
    done
    wait $pid
}

# --- arguments and checks ---------------------------------------------------
PURGE=0
while [ $# -gt 0 ]; do
    case $1 in
        --purge) PURGE=1; shift ;;
        -h | --help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (usage: uninstall.sh [--purge])" ;;
    esac
done
# A folder moved or copied since it was installed: this uninstaller works on the folder it
# is in, and a Core Library inside it moved with it.
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)
if [ "$here" != "$PREFIX" ] && [ -d "$here/push3_opt" ] && [ -e "$here/.ableton-live-linux" ]; then
    case $CORE in "$PREFIX"/*) CORE=$here${CORE#"$PREFIX"} ;; esac
    PREFIX=$here
fi
# The menu entry only when it starts this installation.
[ ! -f "$DESKTOP" ] || grep -qF "$PREFIX/push3_opt/" "$DESKTOP" || DESKTOP="" MIME_XML=""

printf '\n%sAbleton Live %s for Linux: uninstaller%s\n' "$BOLD" "$LIVE_MAJOR" "$RESET"
printf '  Remove from   %s\n\n' "$PREFIX"

step "Checking the installation"
[ "$(id -u)" != 0 ] || die "run this as your normal user; it asks for sudo only for the loader link"
[ -d "$PREFIX/push3_opt" ] || die "no Ableton Live $LIVE_MAJOR installation in $PREFIX"
[ "$PREFIX" != / ] || die "refusing to work on /"
case $(realpath -m -- "$HOME")/ in "$PREFIX"/*) die "refusing: $PREFIX is your home folder or contains it" ;; esac
for p in /proc/[0-9]*; do
    exe=$(readlink "$p/exe" 2>/dev/null) || continue
    case $exe in "$PREFIX"/push3_opt/*) die "Live is still running; quit it first" ;; esac
done
# Live 12: all of ~/.config/Ableton and ~/.local/share/Ableton, and Max for
# Live's settings. When Live 11 (11.3.43) is there too, or for Live 11 itself,
# only this version's own folders: those also hold the other version's
# settings and licence, and Cycling '74 the other Max version's. Live 12's file
# database is Live-files-12300.db and its Max is Max 9; Live 11's Live-files-53.db
# and Max 8.
own_only() {
    local v=$1 db=$2 max=$3 f
    CONFIG="$CONFIG_HOME/Ableton/Live $v"
    DATA="$DATA_HOME/Ableton/Live $v"
    MAX=("$CONFIG_HOME/Cycling '74/$max" "$DATA_HOME/$max")
    for f in "" -shm -wal; do MAX+=("$DATA_HOME/Ableton/Live Database/$db$f"); done
}
if [ "${LIVE_MAJOR:-12}" = 11 ]; then
    own_only 11.3.43 Live-files-53.db "Max 8"
elif [ -e "$CONFIG_HOME/Ableton/Live 11.3.43" ] || [ -e "$DATA_HOME/Ableton/Live 11.3.43" ]; then
    own_only 12.4.7 Live-files-12300.db "Max 9"
else
    CONFIG="$CONFIG_HOME/Ableton"
    DATA="$DATA_HOME/Ableton"
    MAX=("$CONFIG_HOME/Cycling '74" "$DATA_HOME/Max 9") # Max for Live's
fi
if [ -d "$PREFIX/live-home" ]; then
    SETTINGS="$PREFIX/live-home (settings, licence, User Library)"
    CONFIG="" DATA="" MAX=() # all inside live-home
else
    SETTINGS="${CONFIG/#"$HOME"/\~} and ${DATA/#"$HOME"/\~} (settings, licence, file database, Max for Live's settings)"
fi
ok "$(human "$(size_of "$PREFIX")") in ${PREFIX/#"$HOME"/\~}"

# Both questions first, so the rest runs without stopping.
[ $TTY = 0 ] || printf '\e[?25h'
if [ $PURGE = 0 ]; then
    note "Live's settings and licence: $SETTINGS"
    if ask "Delete them too? [y/N]" n; then PURGE=1; fi
fi
UNLINK=0
if [ "$LOADER_LINK" = 1 ] && [ "$(readlink /lib/ld-linux-x86-64.so.2 2>/dev/null)" = /lib64/ld-linux-x86-64.so.2 ]; then
    note "The installer created the link /lib/ld-linux-x86-64.so.2 (other programs may use it now)."
    if ask "Remove it with sudo? [Y/n]" y; then UNLINK=1; fi
fi
UNRULE=0
PUSH_RULE=/etc/udev/rules.d/70-ableton-push.rules
if [ "${UDEV_RULE:-0}" = 1 ] && grep -q "Ableton Live $LIVE_MAJOR, install.sh" "$PUSH_RULE" 2>/dev/null; then
    note "The installer added the Push USB rule $PUSH_RULE."
    if ask "Remove it with sudo? [Y/n]" y; then UNRULE=1; fi
fi
cleanup() { [ $TTY = 0 ] || printf '\e[?25h'; }
trap cleanup EXIT
trap 'exit 130' INT TERM
trap 'exit 129' HUP # the terminal window closed
[ $TTY = 0 ] || printf '\e[?25l' # no cursor blinking over the bars

# --- removing ---------------------------------------------------------------------
step "Removing the Core Library"
case $CORE in
    "$PREFIX"/*) ok "inside the install folder" ;;
    *)
        if [ -d "$CORE" ]; then
            freed=$(size_of "$CORE")
            remove "$CORE" || die "could not delete $CORE"
            ok "$(human "$freed") from ${CORE/#"$HOME"/\~}"
        else
            ok "already gone"
        fi
        ;;
esac

step "Removing the menu entry and file types"
# Before the program: the KWin rule script is in its tools/.
[ ! -x "$PREFIX/tools/kwin-plugin-rule.sh" ] || "$PREFIX/tools/kwin-plugin-rule.sh" --remove >/dev/null 2>&1 || true
[ -z "$DESKTOP" ] || rm -f "$DESKTOP" "$MIME_XML"
command -v update-mime-database >/dev/null && update-mime-database "$DATA_HOME/mime" >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null && update-desktop-database "$(dirname "$DESKTOP")" >/dev/null 2>&1 || true
sed -i "/^x-scheme-handler\/ableton=ableton-live-$LIVE_MAJOR\\.desktop;\\{0,1\\}\$/d" "$CONFIG_HOME/mimeapps.list" 2>/dev/null || true
# The launcher's own handler for ableton:// links (tools/linkhandler.h), when it starts this install.
LINKS="$DATA_HOME/applications/ableton-live-$LIVE_MAJOR-links.desktop"
if [ -f "$LINKS" ] && grep -qF "$PREFIX/push3_opt/push3/AbletonLive$LIVE_MAJOR" "$LINKS"; then
    rm -f "$LINKS"
    for list in "$CONFIG_HOME"/mimeapps.list "$CONFIG_HOME"/*-mimeapps.list; do
        [ -f "$list" ] && sed -i "/^x-scheme-handler\/ableton=ableton-live-$LIVE_MAJOR-links\\.desktop;\\{0,1\\}\$/d" "$list" 2>/dev/null || true
    done
    command -v update-desktop-database >/dev/null && update-desktop-database "$DATA_HOME/applications" >/dev/null 2>&1 || true
fi
ok ".als and .alp files, ableton:// links"

step "Removing the program"
freed=$(size_of "$PREFIX")
if [ $PURGE = 1 ]; then
    mapfile -d '' parts < <(find "$PREFIX" -mindepth 1 -maxdepth 1 -print0)
else
    # An isolated home from an older installation (live-home/) is Live data: kept.
    mapfile -d '' parts < <(find "$PREFIX" -mindepth 1 -maxdepth 1 ! -name live-home -print0)
fi
# Only what the installer put there: anything else in the folder stays.
own=() others=()
for part in "${parts[@]}"; do
    if own_entry "${part##*/}"; then own+=("$part"); else others+=("${part##*/}"); fi
done
remove "${own[@]}" || die "could not delete the program in $PREFIX"
# The launcher's link to the install folder (shim/libpathfix.so), in the runtime folder.
real_prefix=$(readlink -f "$PREFIX" 2>/dev/null || echo "$PREFIX")
for link in "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/ableton-live-*; do
    [ -L "$link" ] && [ "$(readlink "$link")" = "$real_prefix" ] && rm -f "$link"
done
rmdir "$PREFIX" 2>/dev/null || true
ok "$(human "$((freed - $(size_of "$PREFIX")))")"
[ ${#others[@]} -eq 0 ] || note "Kept what the installer did not put in $PREFIX: ${others[*]:0:5}$([ ${#others[@]} -le 5 ] || echo ' ...')"

step "Live's settings, licence, the program loader link and the Push rule"
done_what=()
if [ $PURGE = 1 ]; then
    if [ -n "$CONFIG" ]; then
        freed=$(size_of "$CONFIG" "$DATA" "${MAX[@]}")
        remove "$CONFIG" "$DATA" "${MAX[@]}" || die "could not delete Live's settings"
        done_what+=("settings and licence deleted ($(human "$freed"))")
    else
        done_what+=("settings and licence deleted")
    fi
else
    done_what+=("settings and licence kept")
fi
if [ $UNLINK = 1 ]; then
    [ $TTY = 0 ] || printf '\e[?25h'
    if sudo rm /lib/ld-linux-x86-64.so.2; then
        done_what+=("loader link removed")
    else
        note "The link stays; remove it later with: sudo rm /lib/ld-linux-x86-64.so.2"
    fi
fi
if [ $UNRULE = 1 ]; then
    [ $TTY = 0 ] || printf '\e[?25h'
    if sudo rm "$PUSH_RULE"; then
        sudo udevadm control --reload 2>/dev/null || true
        done_what+=("Push rule removed")
    else
        note "The rule stays; remove it later with: sudo rm $PUSH_RULE"
    fi
fi
ok "$(IFS=,; echo "${done_what[*]}" | sed 's/,/, /g')"

printf '\n%s%sDone.%s Ableton Live %s is uninstalled.\n' "$GREEN" "$BOLD" "$RESET" "$LIVE_MAJOR"
[ ! -d "$PREFIX/live-home" ] || printf 'Your Live data stays in %s\n' "$PREFIX/live-home"
LIBRARY=$(dirname "$CORE")
case $LIBRARY in
    "$PREFIX"/*) ;;
    *) [ ! -d "$LIBRARY" ] || printf 'Your User Library, recordings and projects stay in %s\n' "$LIBRARY" ;;
esac
EOF
} > "$PREFIX/uninstall.sh.part"
chmod +x "$PREFIX/uninstall.sh.part"
# The installation counts as complete from here (see the checks at the start).
mv "$PREFIX/uninstall.sh.part" "$PREFIX/uninstall.sh"
}

# --- update, or what to do -------------------------------------------------------------
if [ $UPDATE = 1 ]; then
    update_main "$UPDATE_DIR"
    exit 0
fi
# Without options: an installation found is offered for an update first.
if [ $ARGC = 0 ]; then
    if [ $INTERACTIVE = 1 ]; then
        choose_install 1
        if [ -n "$CHOSEN" ]; then
            update_main "$CHOSEN"
            exit 0
        fi
    elif [ -z "$(files_of 12)$(files_of 11)" ]; then
        die "the Push 3 image and the Suite installer are not next to install.sh; to update an installation: install.sh --update [FOLDER]"
    fi
fi

# Which Live: --version, else asked (12 by default, or 11 when only its files are
# here), else (no terminal) the one whose files are here, 12 when both or none.
if [ -z "$LIVE_MAJOR" ]; then
    files12=$(files_of 12) files11=$(files_of 11)
    default=12
    [ -n "$files12" ] || [ -z "$files11" ] || default=11
    if [ $INTERACTIVE = 1 ]; then
        ASKED=1
        printf '\n%sWhich Ableton Live do you want to install?%s\n' "$BOLD" "$RESET"
        for v in 12 11; do
            if [ $v = 12 ]; then name="Live 12.4.7 " files=$files12 need="Push3-RecoveryImage-2.4.7.zip and ableton_live_suite_12.4.7_64.zip"
            else name="Live 11.3.43" files=$files11 need="Push3-RecoveryImage-1.2.23.zip and ableton_live_suite_11.3.43_64.zip"; fi
            printf '  %s%s%s  %s%s\n' "$BOLD" "$v" "$RESET" "$name" "$([ $v != "$default" ] || echo "  (default)")"
            if [ -n "$files" ]; then printf '        %sfiles found:%s %s\n' "$GREEN" "$RESET" "$files"
            else printf '        %sfiles not here:%s needs %s\n' "$YELLOW" "$RESET" "$need"; fi
        done
        while :; do
            answer "Type 12 or 11, or press Enter for $default: "
            answer=${answer:-$default}
            case $answer in
                12) files=$files12 ;;
                11) files=$files11 ;;
                *) printf '  %sPlease type 12 or 11.%s\n' "$YELLOW" "$RESET"; continue ;;
            esac
            if [ -z "$files" ] && [ -n "$files12$files11" ]; then
                printf '  %sLive %s'\''s files are not in %s. Put them there first, or choose the other version.%s\n' \
                    "$YELLOW" "$answer" "$SCRIPT_DIR" "$RESET"
                continue
            fi
            LIVE_MAJOR=$answer
            break
        done
    else
        LIVE_MAJOR=$default
    fi
fi
case $LIVE_MAJOR in 12 | 11) ;; *) die "--version takes 12 or 11, not $LIVE_MAJOR" ;; esac
set_version
# Which edition: Live reads it from push3_opt/push3/Installation.cfg ({"variant":
# "Suite"}) and accepts only a licence for that edition; it does not follow the
# licence. The Push image and the Windows Suite installer both say Suite.
# A number is asked, not the name, so the name cannot be mistyped.
EDITIONS=(Suite Standard Intro)
[ $LIVE_MAJOR = 11 ] || EDITIONS+=(Lite) # Live 11 knows no Lite edition
# edition_of ANSWER: the edition for a number or a name (any case), or nothing.
edition_of() {
    local i
    for i in "${!EDITIONS[@]}"; do
        [ "$1" = $((i + 1)) ] || [ "${1,,}" = "${EDITIONS[i],,}" ] && { echo "${EDITIONS[i]}"; return; }
    done
    return 0
}
if [ -n "$EDITION" ]; then
    e=$(edition_of "$EDITION")
    [ -n "$e" ] || die "--edition takes 1 to ${#EDITIONS[@]} (${EDITIONS[*]}) for Live $LIVE_MAJOR, not $EDITION"
    EDITION=$e
elif [ $INTERACTIVE = 1 ]; then
    ASKED=1
    printf '\n%sWhich edition is your licence for?%s\n' "$BOLD" "$RESET"
    printf '  Live accepts only a licence for the edition installed. Without a licence\n'
    printf '  (the trial) choose Suite.\n'
    for i in "${!EDITIONS[@]}"; do
        printf '  %s%s%s  %s%s\n' "$BOLD" $((i + 1)) "$RESET" "${EDITIONS[i]}" "$([ $i != 0 ] || echo "  (default)")"
    done
    while :; do
        answer "Type 1 to ${#EDITIONS[@]}, or press Enter for 1: "
        EDITION=$(edition_of "${answer:-1}")
        [ -n "$EDITION" ] && break
        printf '  %sPlease type a number from 1 to %s.%s\n' "$YELLOW" "${#EDITIONS[@]}" "$RESET"
    done
fi
# The zips are checked before the other questions.
# find_zip WHAT PATTERN SKIP: the one zip in this script's folder that matches.
find_zip() {
    local found=()
    mapfile -t found < <(zips "$2" "$3")
    [ ${#found[@]} -gt 0 ] || die "no $1 in $SCRIPT_DIR (expected $2)"
    [ ${#found[@]} -eq 1 ] || die "several files could be the $1 in $SCRIPT_DIR; keep one:
$(printf '         %s\n' "${found[@]##*/}")"
    echo "${found[0]}"
}
PUSH_ZIP=$(find_zip "Push 3 recovery image for Live $LIVE_MAJOR" "$PUSH_GLOB" "$PUSH_SKIP")
SUITE_ZIP=$(find_zip "Live $LIVE_MAJOR Suite installer" "$SUITE_GLOB" "$SUITE_SKIP")
check_mods
# Normal or portable: --portable, else asked when no --prefix was given.
if [ $INTERACTIVE = 1 ] && [ $PORTABLE = 0 ] && [ $PREFIX_SET = 0 ]; then
    ASKED=1
    printf '\n%sHow do you want to install it?%s\n' "$BOLD" "$RESET"
    printf '  %s1%s  Normal  (default)\n' "$BOLD" "$RESET"
    printf '        In the application menu; settings, licence and libraries in your home folder.\n'
    printf '  %s2%s  Portable\n' "$BOLD" "$RESET"
    printf '        Program, settings, licence and libraries all in one folder you can move or copy\n'
    printf '        (also to another computer). No menu entry; started with start-live.sh in that folder.\n'
    while :; do
        answer "Type 1 or 2, or press Enter for 1: "
        case ${answer:-1} in
            1) break ;;
            2) PORTABLE=1; break ;;
            *) printf '  %sPlease type 1 or 2.%s\n' "$YELLOW" "$RESET" ;;
        esac
    done
fi
[ $PREFIX_SET = 1 ] || PREFIX="$DATA_HOME/ableton-live-$LIVE_MAJOR"
# Portable: program and Live's data (live-home/: settings, licence, Core Library,
# User Library) in one folder next to this script, movable as a whole.
[ $PORTABLE = 0 ] || [ $PREFIX_SET = 1 ] || PREFIX="$SCRIPT_DIR/AbletonLive$LIVE_MAJOR-portable"
# Install folder: --prefix, else asked (the default above on Enter).
if [ $INTERACTIVE = 1 ] && [ $PREFIX_SET = 0 ]; then
    ASKED=1
    PREFIX=$(realpath -m -- "$PREFIX")
    printf '\n%sWhere should Live go?%s\n' "$BOLD" "$RESET"
    printf '  Press Enter for %s%s%s (default),\n' "$BOLD" "${PREFIX/#"$HOME"/\~}" "$RESET"
    printf '  or type another folder: a new or empty one, without spaces or quotes in its path.\n'
    while :; do
        answer "Install folder: "
        dir=${answer:-$PREFIX}
        case $dir in "~") dir=$HOME ;; "~/"*) dir=$HOME/${dir#"~/"} ;; esac
        dir=$(realpath -m -- "$dir")
        problem=$(prefix_problem "$dir")
        if [ -n "$problem" ]; then
            printf '  %s%s%s\n' "$YELLOW" "$problem" "$RESET"
            continue
        fi
        PREFIX=$dir
        break
    done
fi
MODS=$SCRIPT_DIR
PREFIX=$(realpath -m -- "$PREFIX")
# Live registers itself at every start through a shell command that does not
# quote its own folder: a space there gives an error dialog on each start.
case $PREFIX in *[[:space:]\'\"\\\$\`]*) die "the install folder must not contain spaces or quotes: $PREFIX" ;; esac

printf '\n%sAbleton Live %s for Linux: installer%s\n' "$BOLD" "$LIVE_MAJOR" "$RESET"
printf '  Push 3 image  %s\n  Suite         %s\n  Edition       %s\n  Install to    %s%s\n\n' "$PUSH_ZIP" "$SUITE_ZIP" \
    "${EDITION:-as installed before, else Suite}" "$PREFIX" "$([ $PORTABLE = 0 ] || echo ' (portable)')"
# After the questions: the choices above, confirmed before anything is written.
if [ $ASKED = 1 ]; then
    answer "Start the installation? [Y/n] "
    [[ ${answer:-y} == [yY]* ]] || { printf 'Nothing was installed.\n'; exit 0; }
    printf '\n'
fi

step "Checking what the installation needs"
[ "$(id -u)" != 0 ] || die "run this as your normal user; it asks for sudo only for the loader link"
[ "$(uname -m)" = x86_64 ] || die "Live's Linux build is for x86-64 only"
# Live is built for the Push 3's Tiger Lake CPU and uses AVX-512 (with the Ice
# Lake extensions) all through its code. On CPUs without it, the launcher
# preloads the bundled AVX-512 emulator (shim/libavx512emu.so), which needs
# AVX2, FMA and BMI2. A virtual machine can hide flags the CPU has, so the user
# may go on anyway.
cpu_missing=() cpu_base_missing=()
cpu_flags=" $(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null | cut -d: -f2) "
for flag in avx512f avx512cd avx512bw avx512dq avx512vl avx512vbmi avx512_vbmi2 avx512_vnni avx512_vpopcntdq gfni; do
    [[ $cpu_flags == *" $flag "* ]] || cpu_missing+=("$flag")
done
for flag in avx2 fma bmi1 bmi2; do
    [[ $cpu_flags == *" $flag "* ]] || cpu_base_missing+=("$flag")
done
if [ ${#cpu_base_missing[@]} -gt 0 ]; then
    note "This CPU lacks instructions Live needs: ${cpu_base_missing[*]} ${cpu_missing[*]}"
    note "Live needs at least AVX2, FMA and BMI2 (Intel Core 4th gen / AMD Ryzen or newer)."
    note "On other CPUs it stops at start with \"Illegal instruction\"."
    [ $TTY = 0 ] || printf '\e[?25h'
    ask "Install anyway? [y/N]" n || die "this CPU cannot run Live (missing: ${cpu_base_missing[*]})"
    [ $TTY = 0 ] || printf '\e[?25l'
elif [ ${#cpu_missing[@]} -gt 0 ]; then
    note "This CPU has no AVX-512 (${cpu_missing[*]}), which Live is built for."
    note "Live will run through the bundled AVX-512 emulator, using somewhat more CPU."
fi
# Which version: AVX-512 (Live runs natively, no emulator installed) or AVX2
# (with the AVX-512 emulator, which the launcher preloads only on CPUs without
# AVX-512). A portable folder may move to another computer: it gets both.
if [ $CPU_CHOICE = auto ]; then
    if [ $PORTABLE = 1 ]; then CPU_CHOICE=both
    elif [ ${#cpu_missing[@]} -eq 0 ]; then CPU_CHOICE=avx512
    else CPU_CHOICE=avx2; fi
fi
if [ $CPU_CHOICE = avx512 ] && [ ${#cpu_missing[@]} -gt 0 ]; then
    die "--cpu avx512: this CPU lacks ${cpu_missing[*]}; Live would stop with \"Illegal instruction\". Use --cpu avx2"
fi
[ $CPU_CHOICE = avx512 ] || [ -f "$MODS/shim/libavx512emu.so" ] ||
    die "the AVX2 version needs shim/libavx512emu.so next to install.sh (keep the folder as it came)"
case $CPU_CHOICE in
    avx512) ok "AVX-512 version: Live runs natively" ;;
    avx2) ok "AVX2 version: with the AVX-512 emulator" ;;
    both) ok "both versions: the launcher picks per computer" ;;
esac
missing=()
extra_tools=()
[ $LIVE_MAJOR = 12 ] || extra_tools=(cabextract python3) # Live 11's Suite installer (tools/wix-suite-extract.py)
for tool in unzip head realpath du od dd tr awk stat find "${extra_tools[@]}"; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
DEBUGFS=$(command -v debugfs || ls /usr/sbin/debugfs /sbin/debugfs 2>/dev/null | head -1 || true)
[ -n "$DEBUGFS" ] || missing+=(debugfs)
[ ${#missing[@]} -eq 0 ] || die "missing: ${missing[*]}
       install them with:
         $(deps_hint)"
# The install folder holds Live and nothing else: uninstall.sh deletes what the
# installer put there. A folder with other files in it (the home folder, a
# Downloads folder, ...) is refused; the installer's own entries (an earlier or
# unfinished installation) are fine. Home, its parents and system folders never.
# (own_entry and prefix_problem, for the folder question, are further up.)
case $PREFIX/ in
    // | /bin/* | /boot/* | /dev/* | /etc/* | /lib/* | /lib64/* | /proc/* | /run/* | /sbin/* | /sys/* | /usr/* | /var/*)
        die "$PREFIX is a system folder; choose a folder of its own, for example ~/.local/share/ableton-live-$LIVE_MAJOR" ;;
esac
case $(realpath -m -- "$HOME")/ in
    "$PREFIX"/*) die "$PREFIX is your home folder or contains it; choose a folder of its own, for example $PREFIX/ableton-live-$LIVE_MAJOR" ;;
esac
if [ -d "$PREFIX" ]; then
    others=()
    while IFS= read -r -d '' entry; do
        own_entry "${entry##*/}" || others+=("${entry##*/}")
    done < <(find "$PREFIX" -mindepth 1 -maxdepth 1 -print0)
    # An earlier installation there (perhaps by an installer without this check):
    # installing again replaces its uninstall.sh with one that leaves those files.
    if [ ${#others[@]} -gt 0 ] && { [ -e "$PREFIX/push3_opt" ] || [ -e "$PREFIX/.install-program" ]; }; then
        note "$PREFIX also holds other files (${others[*]:0:5}); they stay, also when uninstalling."
        others=()
    fi
    [ ${#others[@]} -eq 0 ] || die "$PREFIX is not empty (${others[*]:0:5}$([ ${#others[@]} -le 5 ] || echo ' ...'))
       uninstall.sh deletes the install folder's contents, so it must be a new or empty folder
       of its own, for example $PREFIX/ableton-live-$LIVE_MAJOR"
elif [ -e "$PREFIX" ]; then
    die "$PREFIX exists and is not a folder"
fi
mkdir -p "$PREFIX" || die "could not create $PREFIX"
[ -w "$PREFIX" ] || die "$PREFIX is not writable"
# Marks the folder as an installation of this package (uninstall.sh checks it).
# A folder holds one Live version: continuing an installation of the other one
# into it would mix the two.
if [ -e "$PREFIX/.ableton-live-linux" ] && ! grep -q "^Ableton Live $LIVE_MAJOR for Linux" "$PREFIX/.ableton-live-linux"; then
    die "$PREFIX holds an installation of another Live version; choose another folder with --prefix"
fi
[ -e "$PREFIX/.ableton-live-linux" ] || echo "Ableton Live $LIVE_MAJOR for Linux install folder; uninstall.sh removes it" > "$PREFIX/.ableton-live-linux"
# One installation into a folder at a time: a second one would take the first
# one's work files away.
exec 9>"$PREFIX/.install.lock"
if command -v flock >/dev/null && ! flock -n 9; then
    die "another installation into $PREFIX is still running"
fi
# What an earlier run left. uninstall.sh is written last, so without it the
# program is from a run that stopped: the parts it finished have a marker
# (.install-program, .install-content) and are kept, the rest is done again.
if [ -e "$PREFIX/push3_opt" ] && [ -e "$PREFIX/uninstall.sh" ]; then
    ! live_running || die "Live is installed in $PREFIX and running; quit it first"
    note "Live is already installed in $PREFIX."
    [ $TTY = 0 ] || printf '\e[?25h'
    if ! grep -q own_entry "$PREFIX/uninstall.sh" 2>/dev/null; then
        note "Its uninstall.sh is from an older installer and deletes everything in $PREFIX."
        note "Installing again replaces it with one that deletes only Live."
    fi
    ask "Install it again? Your settings, licence, libraries and projects stay. [y/N]" n ||
        die "Live is already installed in $PREFIX (to remove it: $PREFIX/uninstall.sh$(grep -q own_entry "$PREFIX/uninstall.sh" 2>/dev/null || echo ', after installing again'))"
    [ $TTY = 0 ] || printf '\e[?25l'
    rm -rf "${PREFIX:?}"/{push3_opt,compat_lib,shim,tools,Options-desktop.txt,start-live.sh,uninstall.sh,.install-program,.install-content} ||
        die "could not remove the installed program from $PREFIX"
elif [ -e "$PREFIX/.install-program" ]; then
    note "Continuing the installation an earlier run did not finish."
elif [ -e "$PREFIX/push3_opt" ] || [ -e "$PREFIX/compat_lib" ]; then
    rm -rf "${PREFIX:?}"/{push3_opt,compat_lib,.install-content} || die "could not remove the unfinished program in $PREFIX"
    note "Removed the unfinished copy of Live an earlier run left; starting it again."
fi
LOG="$PREFIX/install.log"
printf '\n== %s install.sh from %s into %s\n' "$(date '+%F %T')" "$SCRIPT_DIR" "$PREFIX" >> "$LOG" ||
    die "could not write to $PREFIX"
# A live session (a USB system) keeps the home folder in RAM.
case $(findmnt -no FSTYPE --target "$PREFIX" 2>/dev/null || true) in
    tmpfs | overlay)
        note "$PREFIX is in RAM or on an overlay (a live session?): the installation uses that memory and may be gone after a restart." ;;
esac
# Less space when parts are already done (the program is about 4.5 GB of it, Live 11's 2.5 GB).
[ ! -e "$PREFIX/.install-program" ] || NEEDED_GB=$PROGRAM_DONE_GB
[ ! -e "$PREFIX/.install-content" ] || NEEDED_GB=1
free=$(( $(df -Pk "$PREFIX" | awk 'NR==2 {print $4}') * 1024 ))
[ "$free" -ge $((NEEDED_GB * 1000000000)) ] ||
    die "$PREFIX has $(human "$free") free; the installation needs about $NEEDED_GB GB while it runs (about $AFTER_GB GB after)"
rm -rf "$PREFIX"/.install-work* # left by an interrupted run
WORK=$(mktemp -d "$PREFIX/.install-work.XXXXXX")
cleanup() {
    local jobs
    jobs=$(jobs -p)
    [ -z "$jobs" ] || kill $jobs 2>/dev/null || true
    rm -rf "$WORK"
    [ $TTY = 0 ] || printf '\e[?25h'
}
trap cleanup EXIT
stopped() {
    clear_line
    printf '\n%sStopped.%s Run install.sh again to continue; what is finished stays.\n' "$YELLOW$BOLD" "$RESET" >&2
    exit "$1"
}
trap 'stopped 130' INT TERM
trap 'exit 129' HUP # the terminal window closed
[ $TTY = 0 ] || printf '\e[?25l' # no cursor blinking over the bars
ok "$(human "$free") free"

# --- input files --------------------------------------------------------------------
step "Checking the Push 3 image and the Suite installer"
for zip in "$PUSH_ZIP" "$SUITE_ZIP"; do
    [ -r "$zip" ] || die "cannot read $zip"
    unzip -Z1 "$zip" >/dev/null 2>>"$LOG" ||
        die "$(basename "$zip") is not a complete zip: still being copied or downloaded, or damaged (copy it again)"
done
zip_member "$PUSH_ZIP" '\.wic$' >/dev/null || die "$(basename "$PUSH_ZIP") holds no .wic image (is it the Push 3 recovery image?)"
zip_member "$SUITE_ZIP" 'installer.*\.exe$' >/dev/null ||
    die "$(basename "$SUITE_ZIP") holds no Live installer .exe (is it the Windows Live Suite installer?)"
ok "$(basename "$PUSH_ZIP"), $(basename "$SUITE_ZIP")"

# --- Push 3 image: /opt/push3 and libraries ---------------------------------------
cd "$WORK" # debugfs takes output paths without quoting: keep them relative
APP="$PREFIX/push3_opt/push3"
# done_before MARKER: the steps up to MARKER were finished by an earlier run.
done_before() {
    [ -e "$PREFIX/$1" ] || return 1
    ok "done by an earlier run"
}
step "Reading the recovery partition of the Push 3 image"
if ! done_before .install-program; then
    wic=$(zip_member "$PUSH_ZIP" '\.wic$') || die "$PUSH_ZIP holds no .wic image"
    extract_partition "$PUSH_ZIP" "$wic" 2 recovery.img
    ok
fi

step "Unpacking the device image from it"
if ! done_before .install-program; then
    size=$("$DEBUGFS" -R "stat /opt/recovery/CombinedImage.zip" recovery.img 2>>"$LOG" |
        awk '{ for (i = 1; i < NF; i++) if ($i == "Size:") { print $(i + 1); exit } }' || true)
    [ -n "$size" ] || die "no CombinedImage.zip in the recovery partition"
    "$DEBUGFS" -R "dump /opt/recovery/CombinedImage.zip combined.zip" recovery.img >/dev/null 2>>"$LOG" &
    watch_size $! combined.zip "$size" || die "could not copy CombinedImage.zip"
    [ "$(stat -c %s combined.zip)" = "$size" ] || die "could not copy all of CombinedImage.zip"
    rm -f recovery.img
    ok
fi

step "Reading Live's system partition"
if ! done_before .install-program; then
    wic=$(zip_member combined.zip '\.wic$') || die "CombinedImage.zip holds no .wic image"
    extract_partition combined.zip "$wic" 3 rootfs.img
    rm -f combined.zip
    ok
fi

step "Copying Live out of the image"
if ! done_before .install-program; then
    mkdir -p opt lib fallback
    "$DEBUGFS" -R "rdump /opt/push3 opt" rootfs.img >/dev/null 2>>"$LOG" &
    watch_size $! opt "$PUSH3_BYTES" || die "could not copy /opt/push3"
    for f in "${COMPAT[@]}" "${FALLBACK[@]}"; do echo "dump /usr/lib/$f lib/$f"; done > libs.cmd
    "$DEBUGFS" -f libs.cmd rootfs.img >/dev/null 2>>"$LOG"
    rm -f rootfs.img
    [ -x opt/push3/Live ] || die "the image has no /opt/push3/Live"
    rm -rf "${PREFIX:?}"/{push3_opt,compat_lib,.install-content} # the content goes into push3_opt too
    mkdir -p "$PREFIX/push3_opt" "$PREFIX/compat_lib"
    mv opt/push3 "$APP"
    for f in "${COMPAT[@]}"; do
        [ -f "lib/$f" ] || die "the image has no /usr/lib/$f"
        mv "lib/$f" "$PREFIX/compat_lib/$f"
        ln -sf "$f" "$PREFIX/compat_lib/$(soname "$f")"
    done
    for f in "${FALLBACK[@]}"; do
        if [ -f "lib/$f" ]; then mv "lib/$f" "fallback/$(soname "$f")"; fi
    done
    for name in "${XCB_LIBS[@]}"; do copy_lib "$name" "$APP/lib"; done
    copy_lib "$(soname "$NSL")" "$PREFIX/compat_lib"
    touch "$PREFIX/.install-program"
    if [ "$(sha256sum "$APP/Live" | cut -d' ' -f1)" = "$LIVE_SHA256" ]; then
        ok "Live $LIVE_FULL"
    else
        ok
        note "This is not Live $LIVE_FULL (Push 3 $PUSH_VERSION), which the shim was made for; fixes that do not match stay off."
    fi
fi

step "Installing the shim and the launcher"
install_mods
ok

# --- Windows Suite: content the image lacks -----------------------------------------
step "Unpacking the Suite installer"
if ! done_before .install-content; then
    mkdir -p suite
    total=$(unzip -Zt "$SUITE_ZIP" 2>>"$LOG" | grep -o '[0-9]* bytes uncompressed' | cut -d' ' -f1 || true)
    unzip -q "$SUITE_ZIP" -d suite 2>>"$LOG" &
    watch_size $! suite "${total:-6500000000}" || die "could not unpack $SUITE_ZIP"
    exe=$(find suite -maxdepth 2 -name '*Installer.exe' -print -quit)
    [ -n "$exe" ] || die "$SUITE_ZIP holds no Ableton Live installer .exe"
    ok
fi

# bundled_innoextract: the innoextract in tools/bundled (tools/bundled/README),
# when it runs here. It reads Live 12.4's installer (Inno Setup 6.4), which the
# innoextract 1.9 most distributions ship cannot. It links bzip2's library as
# libbz2.so.1.0, which Fedora and openSUSE name libbz2.so.1: a link in the work
# folder stands in for a library missing under its name.
bundled_innoextract() {
    local ie=$PREFIX/tools/bundled/innoextract lib d found
    mkdir -p "$WORK/lib"
    for lib in $(ldd "$ie" 2>/dev/null | awk '/not found/ { print $1 }'); do
        for d in /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /lib64 /usr/lib; do
            found=$(find "$d" -maxdepth 1 -name "${lib%%.so*}.so.*" -print -quit 2>/dev/null || true)
            if [ -n "$found" ]; then
                ln -sf "$found" "$WORK/lib/$lib"
                break
            fi
        done
    done
    LD_LIBRARY_PATH="$WORK/lib" "$ie" --version >/dev/null 2>>"$LOG" || return 1
    echo "$ie"
}

if [ $LIVE_MAJOR = 12 ]; then
    step "Extracting the Core Library, lessons and stem models"
else
    step "Extracting the Core Library and lessons"
fi
if ! done_before .install-content && [ $LIVE_MAJOR = 11 ]; then
    # Live 11's Windows installer is a WiX bundle (Setup.msi inside the .exe, the files
    # in uncompressed cabinets); tools/wix-suite-extract.py (python3 + cabextract)
    # puts the needed files at their Windows paths under suite/x/app.
    rm -rf suite/x
    mkdir -p suite/x/app
    python3 -I "$PREFIX/tools/wix-suite-extract.py" "$(dirname "$exe")" suite/x/app "${SUITE_PARTS[@]}" >/dev/null 2>>"$LOG" &
    watch_size $! suite/x "$SUITE_BYTES" || die "could not unpack $(basename "$exe") (is cabextract installed?)"
    find suite -maxdepth 2 -name '*.cab' -delete
    rm -f "$exe"
    [ -d "suite/x/app/Resources/Core Library" ] || die "the Suite installer has no Core Library (is it the Suite edition?)"
    ok
elif ! done_before .install-content; then
    include=()
    for part in "${SUITE_PARTS[@]}"; do include+=(-I "$part"); done
    # The bundled one first, the system's if that fails (it may be a newer one).
    extractors=()
    if ie=$(bundled_innoextract); then extractors+=("$ie"); fi
    if command -v innoextract >/dev/null; then extractors+=("$(command -v innoextract)"); fi
    [ ${#extractors[@]} -gt 0 ] ||
        die "the bundled innoextract does not run on this system and none is installed; install innoextract 1.10 or newer"
    status=0
    for ie in "${extractors[@]}"; do
        rm -rf suite/x
        printf 'innoextract: %s\n' "$ie" >> "$LOG"
        LD_LIBRARY_PATH="$WORK/lib" "$ie" -s -d suite/x "${include[@]}" "$exe" >/dev/null 2>>"$LOG" &
        if watch_size $! suite/x "$SUITE_BYTES"; then status=0; break; else status=$?; fi
        # Killed (by the kernel when memory ran out): another one would be too.
        [ $status -lt 128 ] || break
    done
    if [ $status -ge 128 ]; then
        die "innoextract was stopped (signal $((status - 128))): did the memory run out? Close other programs and run install.sh again"
    fi
    [ $status = 0 ] || die "innoextract could not read $(basename "$exe")"
    find suite -maxdepth 2 -name '*.bin' -delete
    rm -f "$exe"
    [ -d "suite/x/app/Resources/Core Library" ] || die "the Suite installer has no Core Library (is it the Suite edition?)"
    ok
fi

step "Adding them to Live, with cursors and icon"
if done_before .install-content; then
    CORE=$(cat "$PREFIX/.install-content")
else
    # The Core Library goes where Live looks for it: <Music>/Ableton in your home
    # (tools/import-windows-content.sh), or the isolated live-home/ an earlier
    # installation left in the install folder, which Live then keeps using.
    if [ $PORTABLE = 1 ]; then
        mkdir -p "$PREFIX/live-home" # Live's data stays in the folder (shim/standalone.cpp)
    elif [ -d "$PREFIX/live-home" ]; then
        note "Using the Live data an earlier installation left in $PREFIX/live-home (delete it to use your home)."
    fi
    imported=$(LIVE_MAJOR=$LIVE_MAJOR "$PREFIX/tools/import-windows-content.sh" "$WORK/suite/x/app" 2>>"$LOG") ||
        die "could not copy the Suite content"
    CORE=$(printf '%s\n' "$imported" | sed -n 's/^done: Core Library in \(.*\); Live scans.*/\1/p')
    [ -n "$CORE" ] || die "could not copy the Core Library"
    LD_LIBRARY_PATH="$APP/lib:$PREFIX/compat_lib" QT_PLUGIN_PATH="$APP/plugins" \
        "$PREFIX/tools/make-assets" "$WORK/suite/x/app/Resources" "$APP" >/dev/null 2>>"$LOG" ||
        die "could not make the cursors and icon"
    printf '%s\n' "$CORE" > "$PREFIX/.install-content"
    cd "$PREFIX"
    rm -rf "$WORK"
    ok "$(human "$(du -sb "$PREFIX" "$CORE" | awk '{n += $1} END {print n}')") installed; Core Library in ${CORE/#"$HOME"/\~}"
fi
cd "$PREFIX"
rm -rf "$WORK"
# The edition (asked above): written on every run, so running the installer again
# changes it. Without an answer an earlier installation's edition stays, else Suite.
# A new file, not written into the old one: that may be a hard link to another copy.
cfg=$APP/Installation.cfg
if [ -z "$EDITION" ]; then
    EDITION=$(sed -n 's/.*"variant": *"\([A-Za-z]*\)".*/\1/p' "$cfg" 2>/dev/null | head -1 || true)
    EDITION=$(edition_of "$EDITION")
    EDITION=${EDITION:-Suite}
fi
printf '{\r\n  "variant": "%s"\r\n}\r\n' "$EDITION" > "$cfg.new" && mv -f "$cfg.new" "$cfg" ||
    die "could not write the edition to $cfg"
note "Edition: $EDITION. Live accepts only a licence for this edition; run the installer again to change it."

# --- desktop integration -----------------------------------------------------------
step "Adding Ableton Live $LIVE_MAJOR to the application menu"
write_start_script
DESKTOP="" MIME_XML=""
if [ $PORTABLE = 1 ]; then
    # A menu entry would point into the folder; it moves.
    ok "portable: no menu entry; start Live with $PREFIX/start-live.sh"
else
    write_menu_entry
    ok ".als and .alp files, ableton:// links"
fi

# --- program loader -----------------------------------------------------------------
# Live asks for the loader at /lib/ld-linux-x86-64.so.2, the Push's path. Arch
# has it; most distributions have it only at /lib64.
step "Checking the program loader and Push access"
LOADER_LINK=0
if [ -e /lib/ld-linux-x86-64.so.2 ]; then
    ok "/lib/ld-linux-x86-64.so.2 is there"
else
    note "Live asks for /lib/ld-linux-x86-64.so.2, which this system does not have."
    note "The fix is a link to /lib64/ld-linux-x86-64.so.2 (the same loader); nothing else changes."
    [ $TTY = 0 ] || printf '\e[?25h'
    if [ -e /lib64/ld-linux-x86-64.so.2 ] && ask "Create it now with sudo? [Y/n]" y &&
        sudo ln -s /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2; then
        LOADER_LINK=1
        ok "link created"
    else
        note "Live will not start without it. Create it later with:"
        note "  sudo ln -s /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2"
    fi
fi
# Push 2 / Push 3: Live's companion drives them over raw USB. systemd gives the user at
# the desktop access to devices tagged "uaccess" (logind ACLs); its own list
# (70-av-production.hwdb) has Push 2 but not Push 3. The rule covers every Ableton USB
# device (vendor 2982). 70-: before 73-seat-late.rules, which applies the tag.
UDEV_RULE=0
PUSH_RULE=/etc/udev/rules.d/70-ableton-push.rules
PUSH_RULE_TEXT='SUBSYSTEM=="usb", ATTR{idVendor}=="2982", TAG+="uaccess"'
if [ -e "$PUSH_RULE" ]; then
    ok "Push access: $PUSH_RULE is there"
else
    note "Push 2 and Push 3 need USB access for Live (a udev rule, $PUSH_RULE;"
    note "it is the only file the installer puts outside its folder and your home)."
    [ $TTY = 0 ] || printf '\e[?25h'
    if ask "Add it now with sudo? [Y/n]" y &&
        printf '# Ableton Push 2 / Push 3: the user at the desktop may use them over USB (Ableton Live %s, install.sh)\n%s\n' "$LIVE_MAJOR" "$PUSH_RULE_TEXT" |
        sudo tee "$PUSH_RULE" >/dev/null; then
        sudo udevadm control --reload 2>/dev/null || true
        sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=2982 2>/dev/null || true
        UDEV_RULE=1
        ok "rule added (unplug and replug a connected Push)"
    else
        note "Without it a Push 3 stays dark. Add it later with:"
        note "  echo '$PUSH_RULE_TEXT' | sudo tee $PUSH_RULE"
        note "  sudo udevadm control --reload && sudo udevadm trigger"
    fi
fi

# --- uninstaller ----------------------------------------------------------------------
step "Writing the uninstaller"
write_uninstaller
rm -f "$PREFIX/.install-program" "$PREFIX/.install-content"
write_marker
ok "$PREFIX/uninstall.sh"

if [ ! -e /usr/share/alsa/alsa.conf.d/50-pipewire.conf ] && [ ! -e /etc/alsa/conf.d/50-pipewire.conf ]; then
    printf '\n'
    note "pipewire-alsa seems missing; without it Live takes the sound card from the desktop. Install it with:"
    note "  $(deps_hint)"
fi
if [ $PORTABLE = 1 ]; then
    printf '\n%s%sDone.%s Portable Ableton Live %s in %s (move or copy the whole folder). Start it with:\n' "$GREEN" "$BOLD" "$RESET" "$LIVE_MAJOR" "$PREFIX"
    printf '  %s\n' "$PREFIX/start-live.sh"
else
    printf '\n%s%sDone.%s Start Ableton Live %s from the application menu, or run:\n' "$GREEN" "$BOLD" "$RESET" "$LIVE_MAJOR"
    printf '  %s\n' "$APP/AbletonLive$LIVE_MAJOR"
fi
printf 'Uninstall: %s\n' "$PREFIX/uninstall.sh"
