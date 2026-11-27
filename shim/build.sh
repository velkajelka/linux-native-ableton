#!/bin/sh
# Builds libmenufix.so against the host Qt 5 headers; Live loads it with its bundled Qt 5.15.2.
cd "$(dirname "$0")"
g++ -O2 -fPIC -shared -std=c++17 $(pkg-config --cflags Qt5Widgets Qt5DBus libpipewire-0.3 gio-2.0) \
    menufix.cpp vst3runloop.cpp pluginclose.cpp glsurface.cpp nativemenu.cpp window.cpp alsamidi.cpp alsaaudio.cpp pwdevices.cpp audiodrivers.cpp jackaudio.cpp filedialogs.cpp showinfolder.cpp hook.cpp dragcursor.cpp pointerwarp.cpp scalechange.cpp sleepinhibit.cpp standalone.cpp embedded.cpp glibccompat.cpp livebuild.cpp vst3window11.cpp maxviews.cpp tempfolder.cpp pwgraph.cpp instance.cpp userthemes.cpp chrome.cpp editorcolour.cpp editorplacement.cpp splashzoom.cpp hideexports.cpp hardwareid.cpp licensing.cpp desktopenv.cpp modalrefresh.cpp \
    -o "${1:-libmenufix.so}" -Wl,-soname,libmenufix.so -Wl,-Bsymbolic-functions -Wl,--version-script=qt5.map \
    -Wl,--no-as-needed -lQt5Widgets -lQt5Gui -lQt5DBus -lQt5Core -lasound -lX11 -ldl -lgio-2.0 -lgobject-2.0 -lglib-2.0
# libswupdate.so.0.1 (swupdate-stub.c, from PR #4): stand-in for the Push OS update library the Push 3 companion
# links; plain C, no libc calls. Every entry reports that there is no update service.
gcc -O2 -fPIC -shared swupdate-stub.c -o "$(dirname "${1:-libmenufix.so}")/libswupdate.so.0.1" -Wl,-soname,libswupdate.so.0.1
# libpathfix.so (pathfix.c): plain C, libc only; the launcher preloads it when the install path has a hidden folder.
gcc -O2 -fPIC -shared pathfix.c -o "$(dirname "${1:-libmenufix.so}")/libpathfix.so" -ldl
# libpackmeta.so (packmeta.cpp, packmeta-preload.cpp): libc only, no C++ runtime (gcc links it); the launcher
# preloads it, and Live's helpers keep it (the file indexer reads the packs' Metadata.cfg).
g++ -O2 -fPIC -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -c packmeta.cpp -o "${TMPDIR:-/tmp}/packmeta.$$.o"
g++ -O2 -fPIC -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -c packmeta-preload.cpp -o "${TMPDIR:-/tmp}/packmeta-preload.$$.o"
gcc -shared "${TMPDIR:-/tmp}/packmeta.$$.o" "${TMPDIR:-/tmp}/packmeta-preload.$$.o" -o "$(dirname "${1:-libmenufix.so}")/libpackmeta.so" \
    -Wl,-soname,libpackmeta.so -Wl,--version-script=packmeta.map -Wl,-z,defs -ldl
rm -f "${TMPDIR:-/tmp}/packmeta.$$.o" "${TMPDIR:-/tmp}/packmeta-preload.$$.o"
