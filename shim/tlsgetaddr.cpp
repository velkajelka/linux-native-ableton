// __tls_get_addr for the libraries Live starts with.
//
// With a loader audit module present (the launcher sets LD_AUDIT to
// libhideexports.so), glibc gives the libraries loaded at start a malloc'd
// thread-local block per thread instead of a place in static TLS. glibc's
// x86-64 __tls_get_addr then has a gap: when a thread's TLS generation is
// behind the global one (every thread created after a library with TLS was
// unloaded starts that way) and another thread is inside a TLS allocation at
// that moment, it allocates a fresh block for the module even when the thread
// already has one, and the thread's values in that library revert to zero.
// Qt 5 keeps each thread's QThreadData pointer in such a variable; losing it
// ends in "QThread: Destroyed while thread is still running" and an abort.
// glibc's generic __tls_get_addr checks for an existing block; this does the
// same:
//   - for a module loaded at start (ID below startLimit) whose block exists in
//     the calling thread's vector, return that block plus the offset. Such a
//     module is never unloaded and its slot never changes, so the generation
//     does not matter for it;
//   - everything else (no block yet, libraries opened later) goes to glibc's
//     own __tls_get_addr unchanged.
// The fast path reads glibc's per-thread vector: %fs:8 points at entry 0 (the
// generation), entry -1 holds the number of slots, each entry is 16 bytes with
// the block pointer first, (void *)-1 meaning not allocated yet.
//
// Exported as __tls_get_addr@@GLIBC_2.3 (qt5.map), so the start-up libraries'
// references bind here when the shim is preloaded. Embedded (dlopen()ed
// locally, embedded.cpp) only the shim's own references reach it.
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

extern "C" {
// Read by the assembly below; written once, before Live's code runs (the
// limit) or on the first call that needs glibc (the pointer).
__attribute__((visibility("hidden"))) unsigned long tlsStartLimit = 0;
__attribute__((visibility("hidden"))) void *tlsGlibcGetAddr = nullptr;
__attribute__((visibility("hidden"))) void *tlsResolveGlibc();
__attribute__((visibility("hidden"))) extern char tlsShimGetAddr[];
}

// Uses only %rax, %rdx and the flags on the fast path, needs no stack
// alignment, and tail-jumps to glibc otherwise. Until glibc's function is
// known, a stack-aligned call to tlsResolveGlibc finds it, keeping %rdi.
asm(R"(
    .text
    .globl __tls_get_addr
    .type __tls_get_addr, @function
    .hidden tlsShimGetAddr
    .p2align 4
__tls_get_addr:
tlsShimGetAddr:
    .cfi_startproc
    endbr64
    mov (%rdi), %rax
    cmp tlsStartLimit(%rip), %rax
    jae 1f
    mov %fs:8, %rdx
    cmp -16(%rdx), %rax
    ja 1f
    shl $4, %rax
    mov (%rdx,%rax), %rax
    cmp $-1, %rax
    je 1f
    add 8(%rdi), %rax
    ret
1:
    mov tlsGlibcGetAddr(%rip), %rax
    test %rax, %rax
    jz 2f
    jmp *%rax
2:
    push %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    mov %rsp, %rbp
    .cfi_def_cfa_register %rbp
    and $-16, %rsp
    sub $16, %rsp
    mov %rdi, (%rsp)
    call tlsResolveGlibc
    mov (%rsp), %rdi
    mov %rbp, %rsp
    pop %rbp
    .cfi_def_cfa %rsp, 8
    jmp *%rax
    .cfi_endproc
    .size __tls_get_addr, .-__tls_get_addr
)");

// glibc's __tls_get_addr (in ld.so). RTLD_NEXT finds it for a preloaded or
// DT_NEEDED copy; an embedded copy is not in the global scope, where
// RTLD_DEFAULT finds it. Never this function itself, never null. Two threads
// may both get here first; they find the same address.
void *tlsResolveGlibc()
{
    void *f = dlvsym(RTLD_NEXT, "__tls_get_addr", "GLIBC_2.3");
    if (!f || f == tlsShimGetAddr)
        f = dlvsym(RTLD_DEFAULT, "__tls_get_addr", "GLIBC_2.3");
    if (!f || f == tlsShimGetAddr) {
        static const char msg[] = "libmenufix: glibc's __tls_get_addr not found\n";
        (void)!write(2, msg, sizeof msg - 1);
        abort();
    }
    __atomic_store_n(&tlsGlibcGetAddr, f, __ATOMIC_RELEASE);
    return f;
}

namespace {

constexpr unsigned kMaxModules = 1024;

int noteModule(dl_phdr_info *info, size_t, void *used)
{
    if (info->dlpi_tls_modid && info->dlpi_tls_modid < kMaxModules)
        static_cast<bool *>(used)[info->dlpi_tls_modid] = true;
    return 0;
}

struct TraceModules {
    FILE *file;
    unsigned long limit;
};

int traceModule(dl_phdr_info *info, size_t, void *data)
{
    const auto *t = static_cast<const TraceModules *>(data);
    if (info->dlpi_tls_modid && info->dlpi_tls_modid < t->limit)
        fprintf(t->file, "tls:   module %lu %s\n", static_cast<unsigned long>(info->dlpi_tls_modid),
                info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "(main program)");
    return 0;
}

// The modules loaded at start: the lowest TLS module ID from 1 up that no
// loaded object uses, as glibc computes its own limit at start. Runs before
// Live's code and before any thread exists. A library with TLS that a
// start-up library's constructor opened before this runs would count too;
// with MENUFIX_DEBUG set, the limit and the modules below it go to the trace
// so the list can be checked against the program's start-up libraries.
__attribute__((constructor(101))) void setTlsStartLimit()
{
    static bool used[kMaxModules];
    dl_iterate_phdr(noteModule, used);
    unsigned long limit = 1;
    while (limit < kMaxModules && used[limit])
        ++limit;
    __atomic_store_n(&tlsStartLimit, limit, __ATOMIC_RELEASE);
    if (const char *path = getenv("MENUFIX_DEBUG"))
        if (FILE *f = fopen(path, "a")) {
            fprintf(f, "tls: start limit %lu\n", limit);
            TraceModules t = {f, limit};
            dl_iterate_phdr(traceModule, &t);
            fclose(f);
        }
}

} // namespace
