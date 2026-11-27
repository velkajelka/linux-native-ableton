// libpackmeta.so: the pack metadata fix (packmeta.cpp) as a preload library of its own, libc
// only, so that the programs Live starts get it too. Live does not read a pack's Metadata.cfg
// itself: its file indexer (Ableton Index) does, and Live's helpers run without libmenufix.so
// (menufix.cpp keeps only the emulator, libpathfix.so and this library in their preload).
// The launcher preloads it after libmenufix.so; the shim's fopen (licensing.cpp) reaches this
// one through RTLD_NEXT.
// MENUFIX_PACK_META=0 turns it off.
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

void *nextSymbol(const char *name, const char *version)
{
    return dlvsym(RTLD_NEXT, name, version);
}

// MENUFIX_DEBUG=<file>: the same trace file as the shim's, each line with its process.
void menufixTrace(const char *fmt, int a, int b, int c)
{
    const char *path = getenv("MENUFIX_DEBUG");
    if (!path)
        return;
    static const auto real = reinterpret_cast<FILE *(*)(const char *, const char *)>(nextSymbol("fopen", "GLIBC_2.2.5"));
    if (FILE *f = real(path, "ae")) {
        fprintf(f, "[pid %d] ", int(getpid()));
        fprintf(f, fmt, a, b, c);
        fclose(f);
    }
}

using FopenFn = FILE *(*)(const char *, const char *);
FILE *packMetaFopen(FopenFn real, const char *path, const char *mode); // packmeta.cpp

extern "C" FILE *fopen(const char *path, const char *mode)
{
    static const auto real = reinterpret_cast<FopenFn>(nextSymbol("fopen", "GLIBC_2.2.5"));
    return packMetaFopen(real, path, mode);
}
