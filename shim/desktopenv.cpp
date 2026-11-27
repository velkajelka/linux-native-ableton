// The desktop's own environment for the programs Live opens links with.
//
// Live opens web pages (Authorize with ableton.com, the manual, Help links) with
// xdg-open. That program, and a browser it starts, inherited Live's environment:
// LD_LIBRARY_PATH with Live's own libraries (FFmpeg 5, ICU 70, Qt 5.15), the
// preloaded AVX-512 emulator and libpathfix.so, QT_PLUGIN_PATH with Live's Qt 5
// plug-ins, QT_QPA_PLATFORM=xcb and the other QT_* settings, LC_ALL/LC_NUMERIC,
// and in a portable folder HOME and XDG_*_HOME pointing into live-home, so a
// browser started there had a new, empty profile (not signed in to ableton.com).
//
// A program in kOpeners (or sh -c running one) gets the environment Live was
// started with instead: the values below as they were before the launcher
// (LD_PRELOAD, LD_LIBRARY_PATH: LIVE_DESKTOP_* from tools/launcher.c) and the
// shim (the rest, recorded at constructor priority 101, before standalone.cpp
// changes them at 102) set them. Everything else stays as Live has it.
// The list is built in the parent (at start-up and before each fork), because
// the exec hooks run in the child, where nothing may be allocated.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

extern char **environ;
void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
extern __attribute__((visibility("hidden"))) bool shimActive;

namespace {

const char *const kRestored[] = {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "QT_PLUGIN_PATH",
                                 "QT_QPA_FONTDIR", "QT_QPA_PLATFORM", "QT_QPA_PLATFORMTHEME",
                                 "QT_SCALE_FACTOR_ROUNDING_POLICY", "LC_NUMERIC", "LC_ALL", "PIPEWIRE_ALSA",
                                 "LIVEDEBUGOPTIONS"};
const char *const kFromLauncher[] = {"LD_PRELOAD", "LD_LIBRARY_PATH"};
const char *const kOpeners[] = {"xdg-open",   "xdg-email",  "gio",         "kde-open",   "kde-open5",
                                "kde-open6",  "kioclient",  "kioclient5",  "kioclient6", "exo-open",
                                "gnome-open", "gvfs-open",  "x-www-browser", "sensible-browser", "www-browser"};

struct Saved {
    std::string name, value;
    bool set;
};
std::vector<Saved> &saved()
{
    static std::vector<Saved> s;
    return s;
}
bool recorded;

// Two buffers: a child of vfork() or of a fork() racing a rebuild reads the
// published one while the other is filled.
struct Env {
    std::vector<std::string> strings;
    std::vector<char *> pointers;
};
Env buffers[2];
std::atomic<Env *> published{nullptr};
std::mutex rebuilding;

size_t nameLength(const char *entry)
{
    const char *eq = strchr(entry, '=');
    return eq ? size_t(eq - entry) : strlen(entry);
}

bool managed(const char *entry)
{
    const size_t n = nameLength(entry);
    for (const Saved &s : saved())
        if (s.name.size() == n && !strncmp(entry, s.name.c_str(), n))
            return true;
    return false;
}

const char *baseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool isOpenerName(const char *name, size_t len)
{
    for (const char *o : kOpeners)
        if (strlen(o) == len && !strncmp(name, o, len))
            return true;
    return false;
}

} // namespace

__attribute__((constructor(101))) static void recordDesktopEnv()
{
    for (const char *name : kRestored) {
        // A launcher that already switched HOME keeps the user's in LIVE_USER_HOME.
        const char *value = !strcmp(name, "HOME") && getenv("LIVE_USER_HOME") ? getenv("LIVE_USER_HOME") : getenv(name);
        saved().push_back({name, value ? value : "", value != nullptr});
    }
    for (const char *name : kFromLauncher) {
        const std::string key = std::string("LIVE_DESKTOP_") + name;
        const char *value = getenv(key.c_str());
        if (!value) { // not started by this launcher: leave the variable as it is
            continue;
        }
        const bool set = strcmp(value, "-") != 0;
        saved().push_back({name, set ? value : "", set});
    }
    recorded = true;
}

// Builds the environment for openers from the current one; in the parent only.
void rebuildDesktopEnv()
{
    if (!recorded)
        return;
    std::lock_guard<std::mutex> lock(rebuilding);
    Env *next = published.load() == &buffers[0] ? &buffers[1] : &buffers[0];
    next->strings.clear();
    next->pointers.clear();
    for (char **e = environ; e && *e; ++e)
        if (!managed(*e))
            next->strings.emplace_back(*e);
    for (const Saved &s : saved())
        if (s.set)
            next->strings.push_back(s.name + "=" + s.value);
    for (std::string &s : next->strings)
        next->pointers.push_back(&s[0]);
    next->pointers.push_back(nullptr);
    published.store(next);
}

// The environment for path/argv when it is a program that opens links (or
// sh -c running one), else null. Safe in a forked child (no allocation).
char *const *desktopEnvFor(const char *path, char *const argv[])
{
    if (!shimActive || !path)
        return nullptr;
    const char *name = baseName(path);
    bool opener = isOpenerName(name, strlen(name));
    if (!opener && argv && argv[0] && argv[1] && argv[2] && !strcmp(argv[1], "-c")) {
        const char *cmd = argv[2] + strspn(argv[2], " \t");
        const char *end = cmd + strcspn(cmd, " \t");
        const char *word = cmd;
        for (const char *p = cmd; p < end; ++p)
            if (*p == '/')
                word = p + 1;
        opener = isOpenerName(word, size_t(end - word));
    }
    if (!opener)
        return nullptr;
    Env *env = published.load();
    return env ? env->pointers.data() : nullptr;
}

// The same for system(): the command line itself.
bool commandIsOpener(const char *command)
{
    if (!shimActive || !command)
        return false;
    const char *cmd = command + strspn(command, " \t");
    const char *end = cmd + strcspn(cmd, " \t");
    const char *word = cmd;
    for (const char *p = cmd; p < end; ++p)
        if (*p == '/')
            word = p + 1;
    return isOpenerName(word, size_t(end - word));
}

// From the shim's start-up (menufix.cpp), after the environment is set up.
void installDesktopEnv()
{
    if (!shimActive || !recorded)
        return;
    rebuildDesktopEnv();
    pthread_atfork(rebuildDesktopEnv, nullptr, nullptr);
    menufixTrace("desktopenv: %d variables restored for link openers\n", int(saved().size()));
}
