// libmaxstatfix.so: fixes for Live's Linux Max editor (Resources/Max/Max), preloaded by
// shim/max-editor: btrfs inodes, the liveowner class, shared memory and the zenity file dialog.
//
// btrfs subvolumes:
// Max finds the id of a folder by comparing st_ino alone (Max+0x5c5650, called from the
// parent walk at Max+0x5c1fa0). Every btrfs subvolume root has inode 256 ("/", "/home",
// "/var/tmp" on a default layout), so the parent of "/" can resolve to "/var/tmp", the
// walk goes round in a circle and Max's first database update never ends: the editor
// stays at "Starting Max Editor...". Here a directory with inode 256 gets its device
// number mixed into st_ino, so subvolume roots differ. Nothing else changes.
//
// Max for Live: Live asks the editor for a "liveowner" object (nobox namespace) before it
// shows a device's patcher. The class lives in MaxAudioAPI.so (MaxAudioAPI+0x57bd0 registers
// it), but in this Linux editor nothing calls that function, so every request fails, Max
// logs "loading audio lib!" and Live stays at "Starting Max Editor...". Once MaxAudioAPI.so
// is loaded, the next dlopen on the main thread (Max loads its externals right after it)
// registers the class. The function's first 16 bytes are checked (Max 9.1.5), so another
// build is left alone.
// libc only; preloaded into the editor by shim/max-editor.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "maxshmem.c" // shared memory for the Live link, see there

#define BTRFS_SUBVOLUME_ROOT 256

static void unique(struct stat *st) {
  if (S_ISDIR(st->st_mode) && st->st_ino == BTRFS_SUBVOLUME_ROOT)
    st->st_ino ^= (uint64_t)st->st_dev << 32;
}

int stat(const char *path, struct stat *st) {
  static int (*real)(const char *, struct stat *);
  if (!real)
    real = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "stat");
  const int r = real(path, st);
  if (r == 0)
    unique(st);
  return r;
}

int stat64(const char *path, struct stat64 *st) {
  static int (*real)(const char *, struct stat64 *);
  if (!real)
    real = (int (*)(const char *, struct stat64 *))dlsym(RTLD_NEXT, "stat64");
  const int r = real(path, st);
  if (r == 0)
    unique((struct stat *)st);
  return r;
}

#define LIVEOWNER_INIT 0x57bd0
// cmp qword [rip+0x69da8], 0; je +1; ret; push rbp; push r15; push r14; push r13
static const unsigned char kLiveOwnerInit[] = {0x48, 0x83, 0x3d, 0xa8, 0x9d, 0x06, 0x00, 0x00,
                                               0x74, 0x01, 0xc3, 0x55, 0x41, 0x57, 0x41, 0x56};

static void *audioApi; // MaxAudioAPI.so, once loaded
static int liveOwnerDone;

static void registerLiveOwner(void) {
  struct link_map *map;
  liveOwnerDone = 1;
  if (dlinfo(audioApi, RTLD_DI_LINKMAP, &map) || !map)
    return;
  const unsigned char *fn = (const unsigned char *)map->l_addr + LIVEOWNER_INIT;
  if (memcmp(fn, kLiveOwnerInit, sizeof kLiveOwnerInit))
    return; // another MaxAudioAPI.so build
  ((void (*)(void))fn)();
}

void *dlopen(const char *file, int mode) {
  static void *(*real)(const char *, int);
  if (!real)
    real = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
  if (audioApi && !liveOwnerDone)
    registerLiveOwner();
  void *h = real(file, mode);
  if (h && !audioApi && file) {
    const char *base = strrchr(file, '/');
    if (!strcmp(base ? base + 1 : file, "MaxAudioAPI.so"))
      audioApi = h;
  }
  return h;
}

static void *lookupMax(const char *name) {
  return dlsym(RTLD_DEFAULT, name); // the editor binary exports the sys* API
}

__attribute__((constructor)) static void installShmem(void) { maxShmemInstall(lookupMax); }

// File dialogs: Max runs zenity with "--file-filter= *.amxd" (an empty filter name). zenity 4
// (GTK 4) then never shows its window, so Save in Max hangs. The blanks are dropped
// ("--file-filter=*.amxd"), which zenity shows. Runs in the forked child: no allocation.
#define MAX_ARGS 64

int execvp(const char *file, char *const argv[]) {
  static int (*real)(const char *, char *const[]);
  if (!real)
    real = (int (*)(const char *, char *const[]))dlsym(RTLD_NEXT, "execvp");
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;
  if (strcmp(base, "zenity"))
    return real(file, argv);
  char *args[MAX_ARGS + 1];
  int i = 0;
  for (; argv[i] && i < MAX_ARGS; ++i) {
    char *a = argv[i];
    if (!strncmp(a, "--file-filter=", 14) && a[14] == ' ' && !strchr(a, '|')) {
      char *p = a + 14;
      while (*p == ' ')
        ++p;
      memmove(a + 14, p, strlen(p) + 1); // the child's own copy after fork
    }
    args[i] = a;
  }
  if (argv[i])
    return real(file, argv); // too many to copy: leave as is
  args[i] = 0;
  return real(file, args);
}
