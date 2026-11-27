// Pack contents in the browser: Windows separators in a pack's Metadata.cfg, fixed in memory.
// MENUFIX_PACK_META=0 turns this off.
//
// A factory pack's "Ableton Folder Info/Metadata.cfg" stores its folders and files as
// UTF-16LE strings, and packs built on Windows store them with backslashes
// ("Drums\Kits"). The Core Library's file, and everything Live writes itself, use '/'.
// Live looks up what a pack folder holds by those strings, so on Linux a backslashed one
// never matches: a pack lists its first-level folders and nothing below them. Found by
// 0x2ANHUK (PR #13).
//
// Live's file indexer (Ableton Index) reads the file, through open/open64/fopen/fopen64
// (Live itself: also openat64). This code is built into libpackmeta.so
// (packmeta-preload.cpp), which the launcher preloads into Live and Live's helpers keep.
// When a program opens the file for reading and it has a backslash inside text, it gets a
// copy in memory (memfd) with '/' instead; the pack itself is never changed, so this covers any packs folder, packs
// installed while Live runs, and nothing needs undoing. Same length, so every offset in
// the file stays valid.
//
// Only a 5C 00 pair with a text character on both sides is a separator. The file also
// holds 32-bit lengths in front of each string, and a 92-character string's length is
// 5C 00 00 00: rewriting every 5C 00 pair would turn that 92 into 47 and break the rest of
// the file (the Live 12 Core Library has eight such lengths).
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp

namespace {

constexpr char kSuffix[] = "Ableton Folder Info/Metadata.cfg";

bool enabled()
{
    static const bool on = [] {
        const char *off = getenv("MENUFIX_PACK_META");
        return !(off && *off == '0');
    }();
    return on;
}

bool isPackMeta(const char *path)
{
    if (!path || !enabled())
        return false;
    const size_t n = strlen(path), m = sizeof kSuffix - 1;
    return n >= m && !memcmp(path + n - m, kSuffix, m) && (n == m || path[n - m - 1] == '/');
}

// A UTF-16LE unit that is part of text: not a control character and not 0.
inline bool textUnit(const unsigned char *p)
{
    return (p[0] | p[1] << 8) >= 0x20;
}

} // namespace

// Turns separators inside text into '/'; returns how many. Also used by tests.
long packMetaNormalize(unsigned char *data, size_t size)
{
    long n = 0;
    for (size_t i = 2; i + 3 < size; i++) {
        if (data[i] == 0x5c && data[i + 1] == 0 && textUnit(data + i - 2) && textUnit(data + i + 2)) {
            data[i] = 0x2f;
            n++;
            i++;
        }
    }
    return n;
}

namespace {

// The fixed copy as a memfd, or -1 to open the file itself (nothing to fix or an error).
int fixedCopy(int fd, int flags)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (256 << 20))
        return -1;
    // malloc, not std::vector: libpackmeta.so links no C++ runtime (packmeta-preload.cpp).
    const size_t size = size_t(st.st_size);
    unsigned char *data = static_cast<unsigned char *>(malloc(size));
    if (!data)
        return -1;
    size_t got = 0;
    while (got < size) {
        const ssize_t r = pread(fd, data + got, size - got, off_t(got));
        if (r <= 0) {
            free(data);
            return -1;
        }
        got += size_t(r);
    }
    const long n = packMetaNormalize(data, size);
    const int mem = n ? memfd_create("Metadata.cfg", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0) : -1;
    for (size_t done = 0; mem >= 0 && done < size;) {
        const ssize_t w = write(mem, data + done, size - done);
        if (w <= 0) {
            close(mem);
            free(data);
            return -1;
        }
        done += size_t(w);
    }
    free(data);
    if (mem < 0)
        return -1;
    lseek(mem, 0, SEEK_SET);
    static std::atomic<int> files{0};
    menufixTrace("packmeta: pack metadata %d read with %d separators fixed\n", ++files, int(n));
    return mem;
}

// fd: the file the caller opened. Returns the fd to hand back (fd itself or a memfd).
int swapIfPackMeta(const char *path, int fd, int flags)
{
    if (fd < 0 || (flags & O_ACCMODE) != O_RDONLY || !isPackMeta(path))
        return fd;
    const int saved = errno;
    const int mem = fixedCopy(fd, flags);
    if (mem >= 0) {
        close(fd);
        fd = mem;
    }
    errno = saved;
    return fd;
}

using OpenFn = int (*)(const char *, int, ...);
using OpenatFn = int (*)(int, const char *, int, ...);
using FopenFn = FILE *(*)(const char *, const char *);

mode_t modeArg(int flags, va_list ap)
{
    return (flags & (O_CREAT | O_TMPFILE)) ? mode_t(va_arg(ap, int)) : 0;
}

} // namespace

// fopen (packmeta-preload.cpp) and fopen64.
FILE *packMetaFopen(FopenFn real, const char *path, const char *mode)
{
    if (!mode || mode[0] != 'r' || strchr(mode, '+') || !isPackMeta(path))
        return real(path, mode);
    const int flags = O_RDONLY | (strchr(mode, 'e') ? O_CLOEXEC : 0);
    static const auto open = reinterpret_cast<OpenFn>(nextSymbol("open", "GLIBC_2.2.5"));
    const int fd = swapIfPackMeta(path, open(path, flags), flags);
    if (fd < 0)
        return nullptr;
    FILE *f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

// Versions in shim/packmeta.map: GLIBC_2.2.5 (open, open64, fopen, fopen64), GLIBC_2.4 (openat64).
extern "C" int open(const char *path, int flags, ...)
{
    static const auto real = reinterpret_cast<OpenFn>(nextSymbol("open", "GLIBC_2.2.5"));
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = modeArg(flags, ap);
    va_end(ap);
    return swapIfPackMeta(path, real(path, flags, mode), flags);
}

extern "C" int open64(const char *path, int flags, ...)
{
    static const auto real = reinterpret_cast<OpenFn>(nextSymbol("open64", "GLIBC_2.2.5"));
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = modeArg(flags, ap);
    va_end(ap);
    return swapIfPackMeta(path, real(path, flags, mode), flags);
}

extern "C" int openat64(int dir, const char *path, int flags, ...)
{
    static const auto real = reinterpret_cast<OpenatFn>(nextSymbol("openat64", "GLIBC_2.4"));
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = modeArg(flags, ap);
    va_end(ap);
    return swapIfPackMeta(path, real(dir, path, flags, mode), flags);
}

extern "C" FILE *fopen64(const char *path, const char *mode)
{
    static const auto real = reinterpret_cast<FopenFn>(nextSymbol("fopen64", "GLIBC_2.2.5"));
    return packMetaFopen(real, path, mode);
}
