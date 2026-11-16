// AbletonLive12: starts the Push 3 image's unmodified Live as a desktop app.
//
// Live itself stays as Ableton shipped it. This launcher sits next to it in
// push3_opt/push3/ and
//   - preloads the shim (LD_PRELOAD=../../shim/libmenufix.so, relative to
//     push3_opt/push3, so a folder name with spaces cannot split it;
//     LIVE_SHIM=<path> runs another build). The shim makes the byte patches
//     in memory and sets up the rest of the environment (shim/standalone.cpp);
//   - adds push3_opt/push3/lib and compat_lib to LD_LIBRARY_PATH: Live's own
//     RPATH covers lib/ only, and the loader needs FFmpeg 5 and ICU 70 from
//     compat_lib before the shim runs;
//   - runs Live from push3_opt/push3 with the same arguments, argv[0]
//     included (Live and Qt see the name AbletonLive12, as before).
// Live asks for the loader at /lib/ld-linux-x86-64.so.2, the Push's path.
// Arch has it; Debian, Ubuntu, Mint and Fedora only have
// /lib64/ld-linux-x86-64.so.2, and need a link (shown in a dialog if missing).
//
// Built static (tools/install-launcher.py), so the launcher itself needs no
// loader and no particular glibc.
#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char kLoader[] = "/lib/ld-linux-x86-64.so.2";
static const char kShim[] = "../../shim/libmenufix.so";

// Instance detection files Live leaves behind when it crashes. The next
// launch then starts as a *second* instance, and Live keeps the browser's file
// operations and drop targets off: menus show only "Show in Folder", no New
// Folder/Delete, no drag-and-drop anywhere in the browser. Remove them when no
// Live is running; while one is, they belong to it and Live handles a second
// launch itself.
static const char kInstanceSocket[] = "/tmp/abl-live-detection";
static const char *const kInstanceShm[] = {
    "/dev/shm/abl-live-inter-communication",
    "/dev/shm/sem.abl-live-inter-communication-mutex",
};

static int liveProcessRunning(void)
{
    DIR *dir = opendir("/proc");
    if (!dir)
        return 1; // cannot tell: keep the files
    static const char suffix[] = "/push3_opt/push3/Live";
    struct dirent *entry;
    int found = 0;
    while (!found && (entry = readdir(dir))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        char path[PATH_MAX], exe[PATH_MAX];
        snprintf(path, sizeof path, "/proc/%s/exe", entry->d_name);
        const ssize_t n = readlink(path, exe, sizeof exe - 1);
        if (n <= 0)
            continue;
        exe[n] = '\0';
        const size_t len = strlen(exe), tail = sizeof suffix - 1;
        found = len >= tail && strcmp(exe + len - tail, suffix) == 0;
    }
    closedir(dir);
    return found;
}

// socketPath and shm[] are parameters so the removal itself can be tested
// without touching a running Live's files.
static void removeInstanceFiles(const char *socketPath, const char *const shm[], size_t shmCount)
{
    int removed = unlink(socketPath) == 0;
    for (size_t i = 0; i < shmCount; ++i)
        removed |= unlink(shm[i]) == 0;
    if (removed)
        fprintf(stderr, "AbletonLive12: cleared instance files left by a crashed Live\n");
}

static void clearStaleInstance(void)
{
    if (liveProcessRunning())
        return;
    removeInstanceFiles(kInstanceSocket, kInstanceShm, sizeof kInstanceShm / sizeof kInstanceShm[0]);
}

// stderr, and a dialog when started from a menu (no terminal to read it).
static void report(const char *message)
{
    fprintf(stderr, "AbletonLive12: %s\n", message);
    if (isatty(STDERR_FILENO))
        return;
    char text[1024];
    snprintf(text, sizeof text, "--text=%s", message);
    const pid_t child = fork();
    if (child == 0) {
        execlp("kdialog", "kdialog", "--title", "Ableton Live 12", "--error", message, (char *)NULL);
        execlp("zenity", "zenity", "--error", "--title=Ableton Live 12", "--no-markup", text, (char *)NULL);
        execlp("notify-send", "notify-send", "Ableton Live 12", message, (char *)NULL);
        _exit(127);
    }
    if (child > 0)
        waitpid(child, NULL, 0);
}

// name=first[:previous value]
static void prepend(const char *name, const char *first)
{
    const char *old = getenv(name);
    if (!old || !*old) {
        setenv(name, first, 1);
        return;
    }
    char *value = malloc(strlen(first) + strlen(old) + 2);
    if (!value)
        return;
    sprintf(value, "%s:%s", first, old);
    setenv(name, value, 1);
    free(value);
}

int main(int argc, char **argv)
{
    (void)argc;
    char app[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", app, sizeof app - 1);
    if (n <= 0) {
        perror("AbletonLive12: /proc/self/exe");
        return 1;
    }
    app[n] = '\0';
    *strrchr(app, '/') = '\0';
    if (chdir(app) != 0) {
        perror("AbletonLive12: chdir");
        return 1;
    }
    if (access("Live", X_OK) != 0) {
        report("Live is missing next to AbletonLive12 (push3_opt/push3/Live).");
        return 1;
    }
    if (access(kLoader, X_OK) != 0) {
        report("Live needs the program loader at /lib/ld-linux-x86-64.so.2, which this system "
               "does not have. Create it once with:\n\n"
               "sudo ln -s /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2");
        return 1;
    }
    const char *shim = getenv("LIVE_SHIM");
    if (!shim || !*shim)
        shim = kShim;
    if (access(shim, R_OK) != 0) {
        report("The shim is missing (shim/libmenufix.so next to push3_opt/).");
        return 1;
    }
    prepend("LD_PRELOAD", shim);

    char root[PATH_MAX], libs[3 * PATH_MAX];
    if (!realpath("../..", root)) {
        perror("AbletonLive12: realpath");
        return 1;
    }
    snprintf(libs, sizeof libs, "%s/lib:%s/compat_lib", app, root);
    prepend("LD_LIBRARY_PATH", libs);

    char live[PATH_MAX + 8];
    snprintf(live, sizeof live, "%s/Live", app);
    clearStaleInstance();
    execv(live, argv);
    perror("AbletonLive12: Live");
    return 1;
}
