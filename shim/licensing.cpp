// Authorization that survives reboots, and an account sign-in Live can save.
//
// Hardware code. Live (11 and 12, Push 3 build; Live 12.4.7: 0x1b39a80) derives
// the computer's hardware code from the disk the root folder is on: stat("/"),
// udev_device_new_from_devnum('b', st_dev), then /sys/<devpath>/slaves down to
// the physical device (dm-crypt, LVM) and its ID_SERIAL_SHORT. Without a serial
// it hashes /etc/machine-id with the change time (st_ctim) of "/" instead.
// btrfs (Fedora's and CachyOS's default), overlay and composefs roots report an
// anonymous device (major 0), which udev does not know, so on those systems the
// code depends on the change time of "/": creating, removing or renaming
// anything directly in "/" (sudo mkdir /data, a package that adds a top-level
// folder) makes it a new computer for Ableton. The authorization is lost and
// every new code uses up one of the licence's unlocks ("their unlock limit has
// been reached on this computer").
//
// The lookup of the root's anonymous device is answered with the block device
// that mountinfo names as its source (for "/", else /sysroot, /var, /usr, /etc,
// /home or /boot: composefs/ostree and ZFS roots), so Live finds the disk and
// its serial as it does on ext4. Where no disk with a serial is found (virtual
// disks, some eMMC/SD cards, ZFS), Live's own fallback is kept but stat("/")
// reports the birth time of the root file system as its change time, which
// stays the same until the file system is made anew. A computer with a serial
// on its root disk keeps the code it had. LIVE_HARDWARE_ID=original keeps Live's
// own behaviour.
//
// Account sign-in. Live keeps the Ableton account token in the Push's
// /data/settings/secrets (a std::string built at start, Live 12.4.7 0x9c0cdc0,
// read with std::ifstream at 0x1f0e245 and written with std::ofstream at
// 0x1f0ea51; libstdc++ opens files with fopen). Without /data/settings the token
// is never saved: authorizing with ableton.com fails and the Packs view shows
// "Can't retrieve list (Missing authorization)". fopen of that path opens
// <XDG_CONFIG_HOME or ~/.config>/Ableton/settings/secrets (folder 0700, file
// 0600; a portable folder's live-home/.config) instead. A token saved under
// /data/settings by hand (the sudo mkdir workaround) is copied over once.
#include "hardwareid.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp

namespace {

struct udev;
struct udev_device;
using NewFromDevnumFn = udev_device *(*)(udev *, char, dev_t);

constexpr char kPushSecrets[] = "/data/settings/secrets";

bool original()
{
    static const bool keep = [] {
        const char *v = getenv("LIVE_HARDWARE_ID");
        return v && !strcmp(v, "original");
    }();
    return keep;
}

NewFromDevnumFn realNewFromDevnum()
{
    static const auto fn = reinterpret_cast<NewFromDevnumFn>(nextSymbol("udev_device_new_from_devnum", "LIBUDEV_183"));
    return fn;
}

bool deviceOf(const char *path, dev_t *dev)
{
    struct statx sx;
    if (statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &sx) != 0)
        return false;
    *dev = makedev(sx.stx_dev_major, sx.stx_dev_minor);
    return true;
}

// The block device mountinfo names for the root (see the top), or 0.
dev_t rootBlockDevice()
{
    static const dev_t found = [] {
        std::ifstream file("/proc/self/mountinfo");
        std::stringstream text;
        text << file.rdbuf();
        const char *const points[] = {"/", "/sysroot", "/var", "/usr", "/etc", "/home", "/boot"};
        for (int i = 0; i < 7; ++i) {
            const std::string source = mountSource(text.str(), points[i]);
            struct statx sx;
            if (source.compare(0, 5, "/dev/") == 0 && statx(AT_FDCWD, source.c_str(), 0, STATX_TYPE, &sx) == 0 &&
                S_ISBLK(sx.stx_mode)) {
                menufixTrace("licensing: root disk from mount %d (0 = /)\n", i);
                return makedev(sx.stx_rdev_major, sx.stx_rdev_minor);
            }
        }
        return dev_t(0);
    }();
    return found;
}

// The device Live's lookup ends at: down /sys/dev/block/M:m/slaves as Live does.
dev_t physicalDevice(dev_t dev)
{
    for (int depth = 0; depth < 8; ++depth) {
        const std::string base = "/sys/dev/block/" + std::to_string(major(dev)) + ":" + std::to_string(minor(dev));
        std::string slave;
        if (DIR *dir = opendir((base + "/slaves").c_str())) {
            while (dirent *entry = readdir(dir))
                if (entry->d_name[0] != '.') {
                    slave = entry->d_name;
                    break;
                }
            closedir(dir);
        }
        if (slave.empty())
            break;
        std::ifstream file(base + "/slaves/" + slave + "/dev");
        unsigned maj = 0, min = 0;
        char colon = 0;
        if (!(file >> maj >> colon >> min) || colon != ':')
            break;
        dev = makedev(maj, min);
    }
    return dev;
}

bool hasSerial(dev_t dev)
{
    using NewFn = udev *(*)();
    using PropertyFn = const char *(*)(udev_device *, const char *);
    using DeviceUnrefFn = udev_device *(*)(udev_device *);
    using UnrefFn = udev *(*)(udev *);
    static const auto newUdev = reinterpret_cast<NewFn>(nextSymbol("udev_new", "LIBUDEV_183"));
    static const auto property = reinterpret_cast<PropertyFn>(nextSymbol("udev_device_get_property_value", "LIBUDEV_183"));
    static const auto deviceUnref = reinterpret_cast<DeviceUnrefFn>(nextSymbol("udev_device_unref", "LIBUDEV_183"));
    static const auto unref = reinterpret_cast<UnrefFn>(nextSymbol("udev_unref", "LIBUDEV_183"));
    if (!dev || !newUdev || !property || !deviceUnref || !unref || !realNewFromDevnum())
        return false;
    udev *u = newUdev();
    if (!u)
        return false;
    bool found = false;
    if (udev_device *device = realNewFromDevnum()(u, 'b', physicalDevice(dev))) {
        const char *serial = property(device, "ID_SERIAL_SHORT");
        found = serial && *serial;
        deviceUnref(device);
    }
    unref(u);
    return found;
}

// 0 not yet known, 1 being found (stat calls from inside the lookup see "no"), 2 yes, 3 no.
std::atomic<int> birthTimeState{0};

bool useBirthTime()
{
    int state = birthTimeState.load();
    if (state == 0 && birthTimeState.compare_exchange_strong(state, 1)) {
        dev_t root = 0;
        bool serial = false;
        if (deviceOf("/", &root))
            serial = hasSerial(major(root) == 0 ? rootBlockDevice() : root);
        menufixTrace("licensing: root disk serial %d\n", serial);
        birthTimeState.store(serial ? 3 : 2);
        return !serial;
    }
    return state == 2;
}

bool makeFolders(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            if (mkdir(path.substr(0, i).c_str(), 0700) != 0 && errno != EEXIST)
                return false;
    return access(path.c_str(), W_OK) == 0;
}

using FopenFn = FILE *(*)(const char *, const char *);

bool copyFile(FopenFn open, const char *from, const std::string &to)
{
    FILE *in = open(from, "rb");
    if (!in)
        return false;
    const int fd = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = fd >= 0;
    char buffer[4096];
    size_t n;
    while (ok && (n = fread(buffer, 1, sizeof buffer, in)) > 0)
        ok = write(fd, buffer, n) == ssize_t(n);
    fclose(in);
    if (fd >= 0)
        close(fd);
    if (!ok)
        unlink(to.c_str());
    return ok;
}

FILE *openSecrets(FopenFn open, const char *mode)
{
    const std::string file = secretsFile(getenv("HOME"), getenv("XDG_CONFIG_HOME"));
    if (file.empty())
        return open(kPushSecrets, mode);
    const bool writing = strpbrk(mode, "wa+") != nullptr;
    const bool copy = access(file.c_str(), F_OK) != 0 && access(kPushSecrets, R_OK) == 0;
    if ((writing || copy) && !makeFolders(file.substr(0, file.rfind('/'))))
        return open(kPushSecrets, mode);
    if (copy && copyFile(open, kPushSecrets, file))
        menufixTrace("licensing: account sign-in copied from /data/settings\n");
    FILE *f = open(file.c_str(), mode);
    if (f && writing)
        fchmod(fileno(f), 0600);
    menufixTrace("licensing: account sign-in file opened for %c: %d\n", writing ? 'w' : 'r', f != nullptr);
    return f;
}

} // namespace

// stat("/") from Live (pwdevices.cpp's stat and stat64, and __xstat/__xstat64
// below for Live 11): the root's birth time as its change time when the
// hardware code would otherwise follow the change time (see the top).
void rootStatFix(const char *path, struct stat *st)
{
    if (!path || path[0] != '/' || path[1] || original() || !useBirthTime())
        return;
    struct statx sx;
    if (statx(AT_FDCWD, "/", 0, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME)) {
        st->st_ctim.tv_sec = sx.stx_btime.tv_sec;
        st->st_ctim.tv_nsec = sx.stx_btime.tv_nsec;
        static std::atomic<bool> traced{false};
        if (!traced.exchange(true))
            menufixTrace("licensing: root change time = its birth time\n");
    }
}

// Versions in shim/qt5.map: LIBUDEV_183, GLIBC_2.2.5.
extern "C" udev_device *udev_device_new_from_devnum(udev *u, char type, dev_t devnum)
{
    const NewFromDevnumFn real = realNewFromDevnum();
    if (!real)
        return nullptr;
    dev_t root = 0;
    if (type == 'b' && major(devnum) == 0 && !original() && deviceOf("/", &root) && devnum == root)
        if (const dev_t device = rootBlockDevice())
            return real(u, type, device);
    return real(u, type, devnum);
}

extern "C" FILE *fopen(const char *path, const char *mode)
{
    static const auto real = reinterpret_cast<FopenFn>(nextSymbol("fopen", "GLIBC_2.2.5"));
    if (path && mode && !strcmp(path, kPushSecrets))
        return openSecrets(real, mode);
    return real(path, mode); // libpackmeta.so next, when preloaded
}

extern "C" int __xstat(int version, const char *path, struct stat *buf)
{
    using Fn = int (*)(int, const char *, struct stat *);
    static const auto real = reinterpret_cast<Fn>(nextSymbol("__xstat", "GLIBC_2.2.5"));
    const int result = real(version, path, buf);
    if (result == 0)
        rootStatFix(path, buf);
    return result;
}

extern "C" int __xstat64(int version, const char *path, struct stat64 *buf)
{
    using Fn = int (*)(int, const char *, struct stat64 *);
    static const auto real = reinterpret_cast<Fn>(nextSymbol("__xstat64", "GLIBC_2.2.5"));
    const int result = real(version, path, buf);
    if (result == 0)
        rootStatFix(path, reinterpret_cast<struct stat *>(buf)); // the same layout on x86-64
    return result;
}
