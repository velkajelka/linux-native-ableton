// libhideexports.so: keeps Live's built-in FreeType, HarfBuzz and libpng to
// Live itself.
//
// Live's main program carries its own copies of these libraries and lists their
// functions (FT_*, hb_*, png_*, and FreeType's TrueType interpreter entry
// points TT_New_Context and TT_RunIns) in its dynamic symbol table. The main program
// comes first when the loader resolves a name, so the system's FreeType,
// HarfBuzz, libpng, Cairo and Pango, loaded for Qt and for plug-ins, had many of
// their calls resolved into Live's copies and the rest into their own. Objects
// made by one copy then reached the other, and plug-in editors crashed as soon
// as they drew text.
//
// The launcher loads this file as a loader audit module (LD_AUDIT). The loader
// calls la_objopen for the main program before it resolves any name. When the
// main program is push3_opt/push3/Live, this marks those functions local in the
// program's in-memory symbol table: the loader skips local symbols when it
// resolves names for other objects, so they get the system libraries. Live's
// own code calls its copies directly and is not affected. Live's file on disk
// stays unchanged.
//
// Any other program (Live's helpers, programs Live starts) is left alone, and
// any unexpected layout leaves the table as it is. LIVE_HIDE_EXPORTS=0 tells the
// launcher not to load this module.
//
// Plain C without libc (built -nostdlib): the module lives in the loader's
// audit namespace, and a libc there would be a second copy in the process. It
// exports only la_version and la_objopen, so the loader adds no per-call work.
#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define EXPORT __attribute__((visibility("default")))

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

static int hiddenName(const char *s)
{
    return (s[0] == 'F' && s[1] == 'T' && s[2] == '_') || (s[0] == 'h' && s[1] == 'b' && s[2] == '_')
           || (s[0] == 'p' && s[1] == 'n' && s[2] == 'g' && s[3] == '_') || same(s, "TT_New_Context")
           || same(s, "TT_RunIns");
}

// Whether this process runs .../push3_opt/push3/Live.
static int isLive(void)
{
    static const char kTail[] = "/push3_opt/push3/Live";
    char path[4096];
    const long n = sys3(SYS_readlink, (long)"/proc/self/exe", (long)path, sizeof path);
    const long t = sizeof kTail - 1;
    if (n <= t || n >= (long)sizeof path)
        return 0;
    for (long i = 0; i < t; i++)
        if (path[n - t + i] != kTail[i])
            return 0;
    return 1;
}

static void hideExports(struct link_map *m)
{
    const uintptr_t base = m->l_addr;
    ElfW(Sym) *sym = 0;
    const char *str = 0;
    const uint32_t *gnuHash = 0;
    for (const ElfW(Dyn) *d = m->l_ld; d->d_tag != DT_NULL; d++) {
        uintptr_t p = d->d_un.d_ptr;
        if (p < base) // not yet relocated by the loader
            p += base;
        if (d->d_tag == DT_SYMTAB)
            sym = (ElfW(Sym) *)p;
        else if (d->d_tag == DT_STRTAB)
            str = (const char *)p;
        else if (d->d_tag == DT_GNU_HASH)
            gnuHash = (const uint32_t *)p;
    }
    if (!sym || !str || !gnuHash || !gnuHash[0])
        return;

    // Symbol count: one past the last symbol in the GNU hash chains.
    const uint32_t nBuckets = gnuHash[0], symOffset = gnuHash[1], bloomWords = gnuHash[2];
    const uint32_t *buckets = gnuHash + 4 + bloomWords * (sizeof(ElfW(Addr)) / 4);
    const uint32_t *chain = buckets + nBuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nBuckets; i++)
        if (buckets[i] > last)
            last = buckets[i];
    if (last < symOffset)
        return;
    while (!(chain[last - symOffset] & 1))
        last++;
    const uintptr_t tableStart = (uintptr_t)sym, tableEnd = (uintptr_t)(sym + last + 1);

    // The load segment holding the table, for its protection. A PIE maps its
    // ELF header at the load base.
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)base;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 || eh->e_ident[EI_MAG2] != ELFMAG2
        || eh->e_ident[EI_MAG3] != ELFMAG3 || eh->e_phentsize != sizeof(ElfW(Phdr)))
        return;
    const ElfW(Phdr) *ph = (const ElfW(Phdr) *)(base + eh->e_phoff);
    const uintptr_t page = 4096;
    uintptr_t segStart = 0, segEnd = 0;
    int prot = -1;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        const uintptr_t s = base + ph[i].p_vaddr, e = s + ph[i].p_memsz;
        if (tableStart >= s && tableEnd <= e) {
            segStart = s & ~(page - 1);
            segEnd = (e + page - 1) & ~(page - 1);
            prot = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0)
                   | (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
            break;
        }
    }
    if (prot < 0)
        return;
    const uintptr_t lo = tableStart & ~(page - 1), hi = (tableEnd + page - 1) & ~(page - 1);
    if (lo < segStart || hi > segEnd)
        return;
    if (!(prot & PROT_WRITE) && sys3(SYS_mprotect, (long)lo, (long)(hi - lo), prot | PROT_WRITE) != 0)
        return;

    for (uint32_t i = 1; i <= last; i++) {
        ElfW(Sym) *s = &sym[i];
        const unsigned bind = ELF64_ST_BIND(s->st_info), type = ELF64_ST_TYPE(s->st_info);
        if (s->st_shndx != SHN_UNDEF && type == STT_FUNC && (bind == STB_GLOBAL || bind == STB_WEAK)
            && hiddenName(str + s->st_name))
            s->st_info = ELF64_ST_INFO(STB_LOCAL, type);
    }

    if (!(prot & PROT_WRITE))
        sys3(SYS_mprotect, (long)lo, (long)(hi - lo), prot);
}

EXPORT unsigned int la_version(unsigned int version)
{
    (void)version;
    return LAV_CURRENT;
}

EXPORT unsigned int la_objopen(struct link_map *map, Lmid_t lmid, uintptr_t *cookie)
{
    (void)cookie;
    if (lmid == LM_ID_BASE && map->l_name && !map->l_name[0] && isLive())
        hideExports(map);
    return 0;
}
