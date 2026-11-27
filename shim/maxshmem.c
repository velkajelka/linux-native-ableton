// Shared memory for Max for Live's editor link (Live 12.4.7, Max 9.1.5).
//
// Live and the Max editor (Resources/Max/Max --mfl) exchange a device's audio through named
// shared memory (MaxAudioAPI.so: pluginterface_allocbuffers / sysshmem_open). In the Linux Max
// runtime sysshmem_alloc and sysshmem_open are stubs that fail (both libMaxPlugLib.so, in Live,
// and the Max editor binary), so once the editor takes a device over, Live's audio thread
// reads a null buffer in audiobuffer_client_setschedtime and Live crashes. Here the three
// stubs are replaced with POSIX shm_open/mmap versions; sysshmem_getptr/getsize read the
// handle as {size, ptr} and are kept. A function is only replaced when its bytes are the
// known stub, so a build with a real implementation is left alone.
// Plain C (also compiled as C++ into libmenufix.so); libc only.
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  long size; // read by sysshmem_getsize
  void *ptr; // read by sysshmem_getptr
  int owner;
  char name[256];
} MaxShmem;

static void shmName(char *out, size_t len, const char *name) {
  size_t i = 1;
  out[0] = '/';
  for (; *name && i + 1 < len; ++name)
    out[i++] = *name == '/' ? '_' : *name;
  out[i] = 0;
}

// Names this process created: Live does not close its buffers when it quits, and the
// segments (up to 130 MB each, in RAM) would stay in /dev/shm. Unlinked at exit; a mapping
// that is still in use stays valid.
#define MAX_OWNED 64
static char owned[MAX_OWNED][256];

static void unlinkOwned(void) {
  for (int i = 0; i < MAX_OWNED; ++i)
    if (owned[i][0])
      shm_unlink(owned[i]);
}

static void own(const char *name, int on) {
  static int registered;
  if (on && !registered) {
    registered = 1;
    atexit(unlinkOwned);
  }
  for (int i = 0; i < MAX_OWNED; ++i) {
    if (on ? !owned[i][0] : !strcmp(owned[i], name)) {
      if (on)
        strcpy(owned[i], name);
      else
        owned[i][0] = 0;
      return;
    }
  }
}

static long shmMap(MaxShmem **handle, const char *name, long size, int create) {
  *handle = 0;
  MaxShmem *x = (MaxShmem *)calloc(1, sizeof *x);
  if (!x)
    return -1;
  shmName(x->name, sizeof x->name, name);
  const int fd = shm_open(x->name, create ? O_RDWR | O_CREAT : O_RDWR, 0600);
  if (fd < 0) {
    free(x);
    return -1;
  }
  struct stat st;
  if (create ? ftruncate(fd, size) != 0 : (fstat(fd, &st) != 0 || (size = st.st_size) <= 0)) {
    close(fd);
    if (create)
      shm_unlink(x->name);
    free(x);
    return -1;
  }
  void *p = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    if (create)
      shm_unlink(x->name);
    free(x);
    return -1;
  }
  x->size = size;
  x->ptr = p;
  x->owner = create;
  if (create)
    own(x->name, 1);
  *handle = x;
  return 0;
}

static long maxShmemAlloc(MaxShmem **handle, const char *name, long size, long flags) {
  (void)flags;
  if (!handle || !name || !*name || size <= 0)
    return -1;
  return shmMap(handle, name, size, 1);
}

static long maxShmemOpen(MaxShmem **handle, const char *name, long flags) {
  (void)flags;
  if (!handle || !name || !*name)
    return -1;
  return shmMap(handle, name, 0, 0);
}

static long maxShmemClose(MaxShmem *x) {
  if (!x)
    return -1;
  munmap(x->ptr, (size_t)x->size);
  if (x->owner) {
    shm_unlink(x->name);
    own(x->name, 0);
  }
  free(x);
  return 0;
}

// test rdi,rdi; je; test rsi,rsi; je; mov qword [rdi],0; mov rax,-1; ret
static const unsigned char kStub[] = {0x48, 0x85, 0xff, 0x74, 0x0c, 0x48, 0x85, 0xf6, 0x74,
                                      0x07, 0x48, 0xc7, 0x07, 0x00, 0x00, 0x00, 0x00, 0x48,
                                      0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0xc3};
// test rdi,rdi; je +0xd; push rax; call sysmem_freeptr
static const unsigned char kCloseStub[] = {0x48, 0x85, 0xff, 0x74, 0x0d, 0x50, 0xe8};

static int patchJump(void *fn, const void *expect, size_t len, void *target) {
  unsigned char *p = (unsigned char *)fn;
  if (!p || memcmp(p, expect, len))
    return 0;
  const long page = sysconf(_SC_PAGESIZE);
  unsigned char *start = (unsigned char *)((uintptr_t)p & ~(uintptr_t)(page - 1));
  if (mprotect(start, (size_t)(p + 14 - start), PROT_READ | PROT_WRITE | PROT_EXEC))
    return 0;
  const unsigned char jmp[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
  memcpy(p, jmp, sizeof jmp);
  memcpy(p + 6, &target, sizeof target);
  mprotect(start, (size_t)(p + 14 - start), PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)p, (char *)p + 14);
  return 1;
}

// lookup returns the address of a Max function by name (dlsym on the library that has it).
// Returns how many of the three functions were replaced.
int maxShmemInstall(void *(*lookup)(const char *name)) {
  int n = 0;
  void *alloc = lookup("sysshmem_alloc");
  void *open = lookup("sysshmem_open");
  void *closeFn = lookup("sysshmem_close");
  // Only all or nothing: a half-replaced set would mix handle layouts.
  if (!alloc || !open || !closeFn || memcmp(alloc, kStub, sizeof kStub) ||
      memcmp(open, kStub, sizeof kStub) || memcmp(closeFn, kCloseStub, sizeof kCloseStub))
    return 0;
  n += patchJump(alloc, kStub, sizeof kStub, (void *)&maxShmemAlloc);
  n += patchJump(open, kStub, sizeof kStub, (void *)&maxShmemOpen);
  n += patchJump(closeFn, kCloseStub, sizeof kCloseStub, (void *)&maxShmemClose);
  return n;
}

#ifdef __cplusplus
}
#endif
