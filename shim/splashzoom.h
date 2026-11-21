// Live's Zoom recorded for the splash screen (see shim/window.cpp). Plain C++,
// no Qt, so tests/splash-zoom.sh builds it alone.
#pragma once
#include <string>

// The function the splash wrapper calls through (see shim/window.cpp).
struct ZoomRange {
    float low, high;
};
using CreateWindowFn = void *(*)(void *, int, void *, int, void *, int, ZoomRange);

// The recorded Zoom's file in Live 12.4.7's config folder: under
// XDG_CONFIG_HOME when it is set and not empty (Live's config root then),
// else HOME/.config; empty when neither is usable (nothing is read or written).
std::string splashZoomFile(const char *home, const char *xdgConfigHome);

// The first line of the file as a whole number of percent, if it is 50 to
// 400; else 0 (missing file, empty, text, trailing characters, out of range).
int readZoomPercent(const char *path);

// Writes "<percent>\n" and a note line to <path>.tmp, then renames it over
// <path>. False on any failure; no .tmp file is left behind.
bool writeZoomPercent(const char *path, int percent);

// Calls `create` once with the arguments as given and returns its result. For
// the splash's call (`splash`), a recorded Zoom in `path` other than 100 pins
// the range to it; *pinned is that percent, or 0 when the range was passed as
// given.
void *createWithSplashZoom(CreateWindowFn create, bool splash, const char *path, void *view, int a, void *parameters,
                           int b, void *zoomSource, int c, ZoomRange range, int *pinned);
