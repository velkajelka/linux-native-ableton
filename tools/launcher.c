// AbletonLive12 / AbletonLive11: starts the Push 3 image's unmodified Live as a
// desktop app. One source for both versions: LIVE_MAJOR ("12" by default)
// names it in messages and dialogs; tools/build-launchers.sh builds both.
//
// Live itself stays as Ableton shipped it. This launcher sits next to it in
// push3_opt/push3/ and
//   - preloads the shim (LD_PRELOAD=../../shim/libmenufix.so, relative to
//     push3_opt/push3, so a folder name with spaces cannot split it;
//     LIVE_SHIM=<path> runs another build). The shim makes the byte patches
//     in memory and sets up the rest of the environment (shim/standalone.cpp);
//   - on CPUs without the AVX-512 subset Live is compiled for (it targets the
//     Push 3's Tiger Lake), also preloads the AVX-512 emulator
//     (../../shim/libavx512emu.so), which translates those instructions to
//     AVX2 as they are reached. LIVE_AVX512EMU=1 forces it, =0 turns it off;
//   - loads ../../shim/libhideexports.so as a loader audit module (LD_AUDIT,
//     by full path), which keeps Live's built-in FreeType, HarfBuzz and libpng
//     to Live, so plug-ins and system libraries get the system's
//     (shim/hideexports.c). LIVE_HIDE_EXPORTS=0 turns it off;
//   - adds push3_opt/push3/lib and compat_lib to LD_LIBRARY_PATH: Live's own
//     RPATH covers lib/ only, and the loader needs FFmpeg 5 and ICU 70 (Live
//     11: FFmpeg 58, ICU 66, OpenSSL 1.1) from compat_lib before the shim runs;
//   - runs Live from push3_opt/push3 with the same arguments, argv[0]
//     included (Live and Qt see the name AbletonLive12 / AbletonLive11);
//   - when the install folder's path has a hidden folder in it (the default
//     ~/.local/share/ableton-live-12 or -11), runs Live through a link without one
//     ($XDG_RUNTIME_DIR/ableton-live-<hash>) and preloads shim/libpathfix.so:
//     Live and its indexer skip content in hidden folders, which left the
//     browser's built-in device categories empty (shim/pathfix.c).
//     LIVE_PATHFIX=0 turns this off.
// Live asks for the loader at /lib/ld-linux-x86-64.so.2, the Push's path.
// Arch has it; Debian, Ubuntu, Mint and Fedora only have
// /lib64/ld-linux-x86-64.so.2, and need a link (shown in a dialog if missing).
//
// Built static (tools/build-launchers.sh), so the launcher itself needs no
// loader and no particular glibc.
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LIVE_MAJOR
#define LIVE_MAJOR "12"
#endif
#define APP_NAME "AbletonLive" LIVE_MAJOR
#define APP_TITLE "Ableton Live " LIVE_MAJOR

static const char kLoader[] = "/lib/ld-linux-x86-64.so.2";
static const char kShim[] = "../../shim/libmenufix.so";
static const char kEmu[] = "../../shim/libavx512emu.so";
static const char kPathfix[] = "../../shim/libpathfix.so";

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

// 1: the CPU and the kernel run everything Live uses natively; 0: AVX-512 is
// missing but AVX2 is there (the emulator can help); -1: not even AVX2.
static int cpu_level(void)
{
    unsigned r[4];
    cpuid(0, 0, r);
    const unsigned max = r[0];
    cpuid(1, 0, r);
    const unsigned ecx1 = r[2];
    const int osxsave = (ecx1 >> 27) & 1, fma = (ecx1 >> 12) & 1, avx = (ecx1 >> 28) & 1;
    unsigned long long xcr0 = 0;
    if (osxsave) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = ((unsigned long long)hi << 32) | lo;
    }
    if (max < 7 || !avx || !fma || (xcr0 & 6) != 6)
        return -1;
    cpuid(7, 0, r);
    const unsigned b = r[1], c = r[2];
    const int avx2 = (b >> 5) & 1, bmi1 = (b >> 3) & 1, bmi2 = (b >> 8) & 1;
    if (!avx2 || !bmi1 || !bmi2)
        return -1;
    // F CD BW DQ VL (EBX 16 28 30 17 31), VBMI VBMI2 GFNI VNNI BITALG VPOPCNTDQ (ECX 1 6 8 11 12 14)
    const unsigned need_b = (1u << 16) | (1u << 28) | (1u << 30) | (1u << 17) | (1u << 31);
    const unsigned need_c = (1u << 1) | (1u << 6) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 14);
    if ((b & need_b) == need_b && (c & need_c) == need_c && (xcr0 & 0xe0) == 0xe0)
        return 1;
    return 0;
}

// stderr, and a dialog when started from a menu (no terminal to read it).
static void report(const char *message)
{
    fprintf(stderr, APP_NAME ": %s\n", message);
    if (isatty(STDERR_FILENO))
        return;
    char text[1024];
    snprintf(text, sizeof text, "--text=%s", message);
    const pid_t child = fork();
    if (child == 0) {
        execlp("kdialog", "kdialog", "--title", APP_TITLE, "--error", message, (char *)NULL);
        execlp("zenity", "zenity", "--error", "--title=" APP_TITLE, "--no-markup", text, (char *)NULL);
        execlp("notify-send", "notify-send", APP_TITLE, message, (char *)NULL);
        _exit(127);
    }
    if (child > 0)
        waitpid(child, NULL, 0);
}

// A folder in the path starts with "." (Live does not index content there).
static int hidden_component(const char *path)
{
    return strstr(path, "/.") != NULL;
}

// $XDG_RUNTIME_DIR/ableton-live-<hash of root> -> root, made or corrected.
// The name stays the same for an install, so the paths in Live's database stay
// valid across starts. 0 when there is no runtime folder or the link fails.
static int make_link(const char *root, char *link, size_t size)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/' || hidden_component(runtime))
        return 0;
    unsigned hash = 2166136261u; // FNV-1a
    for (const char *c = root; *c; ++c)
        hash = (hash ^ (unsigned char)*c) * 16777619u;
    if (snprintf(link, size, "%s/ableton-live-%08x", runtime, hash) >= (int)size)
        return 0;
    char target[PATH_MAX];
    const ssize_t n = readlink(link, target, sizeof target - 1);
    if (n > 0) {
        target[n] = '\0';
        if (!strcmp(target, root))
            return 1;
    }
    unlink(link); // an old link to a moved install (or anything else of that name)
    return symlink(root, link) == 0;
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
        perror(APP_NAME ": /proc/self/exe");
        return 1;
    }
    app[n] = '\0';
    *strrchr(app, '/') = '\0';
    if (chdir(app) != 0) {
        perror(APP_NAME ": chdir");
        return 1;
    }
    if (access("Live", X_OK) != 0) {
        report("Live is missing next to " APP_NAME " (push3_opt/push3/Live).");
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
        perror(APP_NAME ": realpath");
        return 1;
    }
    const char *pathfix = getenv("LIVE_PATHFIX");
    char link[PATH_MAX];
    if (hidden_component(root) && !(pathfix && !strcmp(pathfix, "0")) && access(kPathfix, R_OK) == 0
        && make_link(root, link, sizeof link)) {
        char via[PATH_MAX + 32];
        snprintf(via, sizeof via, "%s/push3_opt/push3", link);
        if (chdir(via) == 0) {
            setenv("LIVE_REAL_ROOT", root, 1);
            setenv("LIVE_LINK_ROOT", link, 1);
            // Live 11 and its indexer call glibc's old stat wrappers (shim/pathfix.c).
            if (!strcmp(LIVE_MAJOR, "11"))
                setenv("LIVE_PATHFIX_OLDSTAT", "1", 1);
            snprintf(app, sizeof app, "%s", via);
            snprintf(root, sizeof root, "%s", link);
            char fix[PATH_MAX + 32]; // by full path: helpers Live starts elsewhere load it too
            snprintf(fix, sizeof fix, "%s/shim/libpathfix.so", link);
            prepend("LD_PRELOAD", fix);
        } else if (chdir(app) != 0) {
            perror(APP_NAME ": chdir");
            return 1;
        }
    }

    // Keeps Live's built-in FreeType, HarfBuzz and libpng to Live itself
    // (shim/hideexports.c). By full path: the loader opens it before any chdir.
    const char *hide = getenv("LIVE_HIDE_EXPORTS");
    char audit[PATH_MAX + 32];
    snprintf(audit, sizeof audit, "%s/shim/libhideexports.so", root);
    if (!(hide && !strcmp(hide, "0")) && access(audit, R_OK) == 0)
        prepend("LD_AUDIT", audit);

    const char *force = getenv("LIVE_AVX512EMU");
    const int level = cpu_level();
    const int emulate = force && *force ? *force != '0' : level <= 0;
    if (emulate) {
        if (level < 0) {
            report("This CPU lacks AVX2, FMA or BMI2, which Live needs even with the AVX-512 emulator.");
            return 1;
        }
        if (access(kEmu, R_OK) != 0) {
            report("This CPU has no AVX-512, and the AVX-512 emulator is missing (shim/libavx512emu.so next to push3_opt/).");
            return 1;
        }
        prepend("LD_PRELOAD", kEmu); // first, so its SIGILL handler is in place before anything runs
    }

    snprintf(libs, sizeof libs, "%s/lib:%s/compat_lib", app, root);
    prepend("LD_LIBRARY_PATH", libs);

    char live[PATH_MAX + 8];
    snprintf(live, sizeof live, "%s/Live", app);
    execv(live, argv);
    perror(APP_NAME ": Live");
    return 1;
}
