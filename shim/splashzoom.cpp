#include "splashzoom.h"
#include <cstdio>
#include <cstring>
#include <string>

int readZoomPercent(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[32];
    const bool got = fgets(line, sizeof line, f) != nullptr;
    fclose(f);
    if (!got)
        return 0;
    int value = 0, digits = 0;
    const char *c = line;
    for (; *c >= '0' && *c <= '9' && digits < 4; ++c, ++digits)
        value = value * 10 + (*c - '0');
    if (digits == 0 || (*c != '\n' && *c != '\0'))
        return 0;
    return value >= 50 && value <= 400 ? value : 0;
}

bool writeZoomPercent(const char *path, int percent)
{
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    const bool written = fprintf(f, "%d\nWritten by the Ableton Live Linux shim: Live's Zoom in percent, used to size "
                                    "the splash screen at the next start.\n", percent) > 0;
    if (fclose(f) != 0 || !written || rename(tmp.c_str(), path) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string splashZoomFile(const char *home, const char *xdgConfigHome)
{
    std::string root;
    if (xdgConfigHome && *xdgConfigHome)
        root = xdgConfigHome;
    else if (home && *home)
        root = std::string(home) + "/.config";
    else
        return std::string();
    return root + "/Ableton/Live 12.4.7/LinuxSplashZoom.txt";
}

void *createWithSplashZoom(CreateWindowFn create, bool splash, const char *path, void *view, int a, void *parameters,
                           int b, void *zoomSource, int c, ZoomRange range, int *pinned)
{
    *pinned = 0;
    if (splash && path && *path) {
        const int percent = readZoomPercent(path);
        if (percent && percent != 100) {
            range = {percent / 100.f, percent / 100.f};
            *pinned = percent;
        }
    }
    return create(view, a, parameters, b, zoomSource, c, range);
}
