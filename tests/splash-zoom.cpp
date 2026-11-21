// The recorded Zoom for the splash screen (shim/splashzoom.cpp), run by
// tests/splash-zoom.sh. Expected values: a whole number of percent from 50 to 400 on the
// first line, anything else reads as 0 (no recorded value).
#include "splashzoom.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(const std::string &what, bool ok)
{
    printf("%s %s\n", ok ? "  ok  " : "FAIL:", what.c_str());
    if (!ok)
        ++failures;
}

static void put(const std::string &path, const char *text)
{
    FILE *f = fopen(path.c_str(), "w");
    fputs(text, f);
    fclose(f);
}

static bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// A stand-in for the wrapped function: records what it was called with and
// returns a marker.
struct Call {
    int count;
    void *view, *parameters, *zoomSource;
    int a, b, c;
    ZoomRange range;
} call;
static char created;

static void *captureCreate(void *view, int a, void *parameters, int b, void *zoomSource, int c, ZoomRange range)
{
    call.count++;
    call.view = view, call.a = a, call.parameters = parameters, call.b = b;
    call.zoomSource = zoomSource, call.c = c, call.range = range;
    return &created;
}

// Calls the wrapper with fixed arguments and a 100 % range; true when the
// stand-in got exactly those arguments with `expected` as the range, once,
// and the wrapper returned its result and reported `expectedPin`.
static bool forwards(bool splash, const char *path, ZoomRange expected, int expectedPin)
{
    call = Call{};
    char view, parameters, zoomSource;
    int pinned = -1;
    void *result = createWithSplashZoom(&captureCreate, splash, path, &view, 2, &parameters, 0, &zoomSource, 1,
                                        ZoomRange{1.f, 1.f}, &pinned);
    return result == &created && call.count == 1 && call.view == &view && call.a == 2 && call.parameters == &parameters
        && call.b == 0 && call.zoomSource == &zoomSource && call.c == 1 && call.range.low == expected.low
        && call.range.high == expected.high && pinned == expectedPin;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("FAIL: usage: splash-zoom <temporary folder>\n");
        return 2;
    }
    const std::string dir = argv[1], file = dir + "/LinuxSplashZoom.txt";

    check("missing file reads 0", readZoomPercent(file.c_str()) == 0);
    const struct {
        const char *text;
        int expected;
    } cases[] = {{"110\n", 110}, {"100\n", 100}, {"110", 110}, {"50\n", 50}, {"400\n", 400},
                 {"110\nWritten by someone\n", 110}, {"", 0}, {"\n", 0}, {"abc\n", 0},
                 {"110abc\n", 0}, {"0\n", 0}, {"49\n", 0}, {"401\n", 0}, {"-110\n", 0},
                 {"nan\n", 0}, {"110.5\n", 0}, {" 110\n", 0}, {"99999999999999999999\n", 0}};
    for (const auto &c : cases) {
        put(file, c.text);
        std::string shown = c.text;
        for (size_t i = 0; (i = shown.find('\n', i)) != std::string::npos;)
            shown.replace(i, 1, "\\n");
        check("\"" + shown + "\" reads " + std::to_string(c.expected), readZoomPercent(file.c_str()) == c.expected);
    }

    unlink(file.c_str());
    check("write 110 succeeds", writeZoomPercent(file.c_str(), 110));
    check("written 110 reads back 110", readZoomPercent(file.c_str()) == 110);
    check("no .tmp left after a write", !exists(file + ".tmp"));
    check("write 125 over it succeeds", writeZoomPercent(file.c_str(), 125));
    check("second write replaces the first (reads 125)", readZoomPercent(file.c_str()) == 125);

    const std::string missing = dir + "/no-such-folder/LinuxSplashZoom.txt";
    check("write into a missing folder fails", !writeZoomPercent(missing.c_str(), 110));
    check("no .tmp left in a missing folder", !exists(missing + ".tmp"));

    // The wrapper: the arguments always reach the wrapped function once, and
    // its result comes back; only the splash with a recorded Zoom other than
    // 100 gets a pinned range.
    unlink(file.c_str());
    check("splash, no file: range passed as given", forwards(true, file.c_str(), {1.f, 1.f}, 0));
    for (const char *text : {"", "\n", "abc\n", "110abc\n", "0\n", "49\n", "401\n", "-110\n", "100\n"}) {
        put(file, text);
        const std::string shown(text, strcspn(text, "\n"));
        check("splash, file \"" + shown + "\": range passed as given", forwards(true, file.c_str(), {1.f, 1.f}, 0));
    }
    put(file, "110\n");
    check("splash, file 110: range pinned to 1.1", forwards(true, file.c_str(), {1.1f, 1.1f}, 110));
    check("other window, file 110: range passed as given", forwards(false, file.c_str(), {1.f, 1.f}, 0));
    check("splash, empty path (no HOME): range passed as given", forwards(true, "", {1.f, 1.f}, 0));
    check("splash, null path: range passed as given", forwards(true, nullptr, {1.f, 1.f}, 0));

    // Where the recorded Zoom lives: Live's config root is
    // XDG_CONFIG_HOME when it is set and not empty, else HOME/.config.
    const std::string tail = "/Ableton/Live 12.4.7/LinuxSplashZoom.txt";
    check("HOME only", splashZoomFile("/h", nullptr) == "/h/.config" + tail);
    check("empty XDG_CONFIG_HOME", splashZoomFile("/h", "") == "/h/.config" + tail);
    check("XDG_CONFIG_HOME set", splashZoomFile("/h", "/x/conf") == "/x/conf" + tail);
    check("XDG_CONFIG_HOME without HOME", splashZoomFile(nullptr, "/x/conf") == "/x/conf" + tail);
    check("neither: no file", splashZoomFile(nullptr, nullptr).empty());
    check("empty HOME, no XDG_CONFIG_HOME: no file", splashZoomFile("", nullptr).empty());

    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: recorded splash Zoom reads, rejects and writes as specified\n");
    return 0;
}
