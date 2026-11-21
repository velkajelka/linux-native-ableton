#!/bin/sh
# Builds libmenufix.so against the host Qt 5 headers; Live loads it with its bundled Qt 5.15.2.
cd "$(dirname "$0")"
g++ -O2 -fPIC -shared -std=c++17 $(pkg-config --cflags Qt5Widgets Qt5DBus libpipewire-0.3) \
    menufix.cpp vst3runloop.cpp pluginclose.cpp glsurface.cpp nativemenu.cpp window.cpp alsamidi.cpp alsaaudio.cpp pwdevices.cpp audiodrivers.cpp jackaudio.cpp filedialogs.cpp hook.cpp dragcursor.cpp pointerwarp.cpp scalechange.cpp sleepinhibit.cpp standalone.cpp embedded.cpp glibccompat.cpp livebuild.cpp vst3window11.cpp maxviews.cpp tempfolder.cpp pwgraph.cpp instance.cpp userthemes.cpp wheeltrace.cpp wheelvalues.cpp maxwheel.cpp \
    -o "${1:-libmenufix.so}" -Wl,-soname,libmenufix.so -Wl,-Bsymbolic-functions -Wl,--version-script=qt5.map \
    -Wl,--no-as-needed -lQt5Widgets -lQt5Gui -lQt5DBus -lQt5Core -lasound -lX11 -ldl
# libswupdate.so.0.1 (swupdate-stub.c, from PR #4): stand-in for the Push OS update library the Push 3 companion
# links; plain C, no libc calls. Every entry reports that there is no update service.
gcc -O2 -fPIC -shared swupdate-stub.c -o "$(dirname "${1:-libmenufix.so}")/libswupdate.so.0.1" -Wl,-soname,libswupdate.so.0.1
# libpathfix.so (pathfix.c): plain C, libc only; the launcher preloads it when the install path has a hidden folder.
gcc -O2 -fPIC -shared pathfix.c -o "$(dirname "${1:-libmenufix.so}")/libpathfix.so" -ldl
