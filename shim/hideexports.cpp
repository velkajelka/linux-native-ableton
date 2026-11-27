// Keeps Live's built-in FreeType, HarfBuzz, libpng and OpenSSL to Live itself.
//
// Live's main program carries its own copies of these libraries and lists their
// functions (FT_*, hb_*, png_*, and FreeType's TrueType interpreter entry points
// TT_New_Context and TT_RunIns) in its dynamic symbol table. The main program
// comes first when the loader resolves a name, so the system's FreeType,
// HarfBuzz, libpng, Cairo and Pango, loaded for Qt and for plug-ins, had many of
// their calls resolved into Live's copies and the rest into their own. Objects
// made by one copy then reached the other, and plug-in editors crashed as soon
// as they drew text.
//
// In memory only, before Live's own code runs (Live's file is not changed):
//   1. those functions are marked local in the main program's symbol table.
//      The loader skips local symbols, so every library loaded from now on
//      (plug-ins and what they load) gets the system's. Live's own code calls
//      its copies directly (it has no relocations against them);
//   2. the libraries loaded with Live (bound at start, most of them with
//      BIND_NOW) had already bound some of these names into Live: each such
//      slot (GOT or data pointer) is bound again, to what the loader now finds
//      for that name and version.
// The same for Live's built-in OpenSSL 1.1.1: every function the system's
// libssl/libcrypto (loaded with Live: OpenSSL 3) also define. Before, a plug-in
// whose libcurl or libssl went through the system's OpenSSL got Live's 1.1.1
// functions for many of those calls (a test library that calls the system's
// OpenSSL_version got "OpenSSL 1.1.1h"). The Max runtime's copies of libcurl
// and OpenSSL are handled the same way once it is loaded (below; Vital's sign-in).
// Nothing Live ships (Max, Python modules) uses Live's copies by name.
// Live 12.4.7 and 11.3.43 (Live 11 links OpenSSL 1.1 as a library, so only its fonts
// libraries and the Max runtime's copies are hidden there); LIVE_HIDE_EXPORTS=0 turns it
// off, =fonts leaves OpenSSL out.
// MENUFIX_DEBUG traces "hideexports: <hidden> hidden, <rebound> bound again,
// <left> left" and the number of OpenSSL names.
#include "livebuild.h"
#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string_view>
#include <unordered_set>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

// Names the system's OpenSSL (libssl/libcrypto loaded with Live) defines: Live's
// built-in OpenSSL 1.1.1 has 4,152 of them (Live 12.4.7 against OpenSSL 3).
// (A function's static: hideLiveExports runs at constructor priority 101, before globals are built.)
std::unordered_set<std::string_view> &opensslNames()
{
    static std::unordered_set<std::string_view> names;
    return names;
}

bool hiddenName(const char *s)
{
    return !strncmp(s, "FT_", 3) || !strncmp(s, "hb_", 3) || !strncmp(s, "png_", 4) || !strcmp(s, "TT_New_Context")
           || !strcmp(s, "TT_RunIns") || opensslNames().count(s);
}

// What an object's dynamic section points at. The loader has relocated most
// d_ptr entries in memory; any that are still file offsets get the base added.
struct Dynamic {
    ElfW(Sym) *sym = nullptr;
    const char *str = nullptr;
    const uint32_t *gnuHash = nullptr;
    const ElfW(Rela) *rela = nullptr;
    size_t relaSize = 0;
    const ElfW(Rela) *plt = nullptr;
    size_t pltSize = 0;
    bool pltIsRela = true;
    const ElfW(Half) *versym = nullptr;
    const ElfW(Verneed) *verneed = nullptr;
    size_t verneedCount = 0;
};

Dynamic readDynamic(uintptr_t base, const ElfW(Dyn) *d)
{
    Dynamic r;
    for (; d && d->d_tag != DT_NULL; ++d) {
        uintptr_t p = d->d_un.d_ptr;
        if (p < base)
            p += base;
        switch (d->d_tag) {
        case DT_SYMTAB: r.sym = reinterpret_cast<ElfW(Sym) *>(p); break;
        case DT_STRTAB: r.str = reinterpret_cast<const char *>(p); break;
        case DT_GNU_HASH: r.gnuHash = reinterpret_cast<const uint32_t *>(p); break;
        case DT_RELA: r.rela = reinterpret_cast<const ElfW(Rela) *>(p); break;
        case DT_RELASZ: r.relaSize = d->d_un.d_val; break;
        case DT_JMPREL: r.plt = reinterpret_cast<const ElfW(Rela) *>(p); break;
        case DT_PLTRELSZ: r.pltSize = d->d_un.d_val; break;
        case DT_PLTREL: r.pltIsRela = d->d_un.d_val == DT_RELA; break;
        case DT_VERSYM: r.versym = reinterpret_cast<const ElfW(Half) *>(p); break;
        case DT_VERNEED: r.verneed = reinterpret_cast<const ElfW(Verneed) *>(p); break;
        case DT_VERNEEDNUM: r.verneedCount = d->d_un.d_val; break;
        }
    }
    return r;
}

struct Object {
    uintptr_t base;
    const ElfW(Phdr) *phdr;
    int phnum;
    const char *name;
};

int collect(dl_phdr_info *info, size_t, void *out)
{
    static_cast<std::vector<Object> *>(out)->push_back({info->dlpi_addr, info->dlpi_phdr, info->dlpi_phnum, info->dlpi_name});
    return 0;
}

const ElfW(Dyn) *dynamicOf(const Object &o)
{
    for (int i = 0; i < o.phnum; ++i)
        if (o.phdr[i].p_type == PT_DYNAMIC)
            return reinterpret_cast<const ElfW(Dyn) *>(o.base + o.phdr[i].p_vaddr);
    return nullptr;
}

// Changes the bytes [start, end) of `o` with `write`, making the pages
// writable for that if they are not; the pages get their protection back.
template <typename Fn> bool withWritable(const Object &o, uintptr_t start, uintptr_t end, Fn write)
{
    int prot = -1;
    for (int i = 0; i < o.phnum; ++i) {
        const ElfW(Phdr) &ph = o.phdr[i];
        const uintptr_t s = o.base + ph.p_vaddr, e = s + ph.p_memsz;
        if (ph.p_type == PT_LOAD && start >= s && end <= e)
            prot = (ph.p_flags & PF_R ? PROT_READ : 0) | (ph.p_flags & PF_W ? PROT_WRITE : 0) | (ph.p_flags & PF_X ? PROT_EXEC : 0);
    }
    if (prot < 0)
        return false;
    // RELRO: the loader made it read only after relocating.
    for (int i = 0; i < o.phnum; ++i) {
        const ElfW(Phdr) &ph = o.phdr[i];
        const uintptr_t s = o.base + ph.p_vaddr, e = s + ph.p_memsz;
        if (ph.p_type == PT_GNU_RELRO && start < e && end > s)
            prot &= ~PROT_WRITE;
    }
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    const uintptr_t lo = start & ~(page - 1), hi = (end + page - 1) & ~(page - 1);
    if (!(prot & PROT_WRITE) && mprotect(reinterpret_cast<void *>(lo), hi - lo, prot | PROT_WRITE) != 0)
        return false;
    write();
    if (!(prot & PROT_WRITE))
        mprotect(reinterpret_cast<void *>(lo), hi - lo, prot);
    return true;
}

// One past the last symbol in an object's GNU hash chains (its dynamic symbol count), or 0.
uint32_t symbolCount(const Dynamic &d)
{
    if (!d.gnuHash || !d.gnuHash[0])
        return 0;
    const uint32_t nBuckets = d.gnuHash[0], symOffset = d.gnuHash[1], bloomWords = d.gnuHash[2];
    const uint32_t *buckets = d.gnuHash + 4 + bloomWords * (sizeof(ElfW(Addr)) / 4);
    const uint32_t *chain = buckets + nBuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nBuckets; ++i)
        last = std::max(last, buckets[i]);
    if (last < symOffset)
        return 0;
    while (!(chain[last - symOffset] & 1))
        ++last;
    return last + 1;
}

// The functions the loaded libssl.so.N and libcrypto.so.N define, into opensslNames.
void collectOpenSSLNames(const std::vector<Object> &objects)
{
    for (size_t o = 1; o < objects.size(); ++o) {
        const char *slash = strrchr(objects[o].name, '/');
        const char *file = slash ? slash + 1 : objects[o].name;
        if (strncmp(file, "libssl.so.", 10) && strncmp(file, "libcrypto.so.", 13))
            continue;
        const Dynamic d = readDynamic(objects[o].base, dynamicOf(objects[o]));
        const uint32_t n = symbolCount(d);
        for (uint32_t i = 1; d.sym && d.str && i < n; ++i) {
            const ElfW(Sym) &s = d.sym[i];
            const unsigned bind = ELF64_ST_BIND(s.st_info);
            if (s.st_shndx != SHN_UNDEF && ELF64_ST_TYPE(s.st_info) == STT_FUNC && (bind == STB_GLOBAL || bind == STB_WEAK))
                opensslNames().insert(d.str + s.st_name);
        }
    }
}

// Step 1: marks the functions local in the main program; their addresses go to `hidden` (sorted).
int hideInMain(const Object &main, std::vector<uintptr_t> &hidden)
{
    const Dynamic d = readDynamic(main.base, dynamicOf(main));
    const uint32_t count = symbolCount(d);
    if (!d.sym || !d.str || !count)
        return 0;
    std::vector<uint32_t> found;
    for (uint32_t i = 1; i < count; ++i) {
        const ElfW(Sym) &s = d.sym[i];
        const unsigned bind = ELF64_ST_BIND(s.st_info);
        if (s.st_shndx != SHN_UNDEF && ELF64_ST_TYPE(s.st_info) == STT_FUNC && (bind == STB_GLOBAL || bind == STB_WEAK)
            && hiddenName(d.str + s.st_name))
            found.push_back(i);
    }
    if (found.empty())
        return 0;
    const uintptr_t start = reinterpret_cast<uintptr_t>(d.sym + found.front()),
                    end = reinterpret_cast<uintptr_t>(d.sym + found.back() + 1);
    if (!withWritable(main, start, end, [&] {
            for (uint32_t i : found)
                d.sym[i].st_info = ELF64_ST_INFO(STB_LOCAL, ELF64_ST_TYPE(d.sym[i].st_info));
        }))
        return 0;
    for (uint32_t i : found)
        hidden.push_back(main.base + d.sym[i].st_value);
    std::sort(hidden.begin(), hidden.end());
    return int(found.size());
}

// The version `o` asks for with its symbol `index`, or null.
const char *versionOf(const Dynamic &d, size_t index)
{
    if (!d.versym || !d.verneed)
        return nullptr;
    const unsigned want = d.versym[index] & 0x7fff;
    if (want <= 1)
        return nullptr;
    const ElfW(Verneed) *vn = d.verneed;
    for (size_t n = 0; n < d.verneedCount; ++n) {
        const auto *aux = reinterpret_cast<const ElfW(Vernaux) *>(reinterpret_cast<const char *>(vn) + vn->vn_aux);
        for (unsigned a = 0; a < vn->vn_cnt; ++a) {
            if (aux->vna_other == want)
                return d.str + aux->vna_name;
            aux = reinterpret_cast<const ElfW(Vernaux) *>(reinterpret_cast<const char *>(aux) + aux->vna_next);
        }
        if (!vn->vn_next)
            break;
        vn = reinterpret_cast<const ElfW(Verneed) *>(reinterpret_cast<const char *>(vn) + vn->vn_next);
    }
    return nullptr;
}

struct Counts {
    int rebound = 0, left = 0;
};

// Step 2 for one object: every slot bound to one of `hidden` is bound again.
// With `dryRun`, only counts the slots that still point into Live.
void rebind(const Object &o, const std::vector<uintptr_t> &hidden, bool dryRun, Counts &counts,
            bool (*named)(const char *) = hiddenName)
{
    const Dynamic d = readDynamic(o.base, dynamicOf(o));
    if (!d.sym || !d.str)
        return;
    auto isHidden = [&](uintptr_t a) { return std::binary_search(hidden.begin(), hidden.end(), a); };
    auto table = [&](const ElfW(Rela) *r, size_t bytes) {
        for (size_t i = 0; r && i < bytes / sizeof(ElfW(Rela)); ++i) {
            const unsigned type = ELF64_R_TYPE(r[i].r_info);
            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT && type != R_X86_64_64)
                continue;
            const size_t index = ELF64_R_SYM(r[i].r_info);
            if (!index || !named(d.str + d.sym[index].st_name))
                continue;
            auto *slot = reinterpret_cast<uintptr_t *>(o.base + r[i].r_offset);
            const uintptr_t addend = type == R_X86_64_64 ? uintptr_t(r[i].r_addend) : 0;
            if (!isHidden(*slot - addend))
                continue;
            if (dryRun) {
                ++counts.left;
                continue;
            }
            const char *name = d.str + d.sym[index].st_name;
            const char *version = versionOf(d, index);
            void *to = version ? dlvsym(RTLD_DEFAULT, name, version) : dlsym(RTLD_DEFAULT, name);
            if (!to && version) // a definition without versions satisfies a versioned reference
                to = dlsym(RTLD_DEFAULT, name);
            if (!to || isHidden(reinterpret_cast<uintptr_t>(to))) {
                ++counts.left;
                continue;
            }
            const uintptr_t at = reinterpret_cast<uintptr_t>(slot);
            if (withWritable(o, at, at + sizeof *slot, [&] { *slot = reinterpret_cast<uintptr_t>(to) + addend; }))
                ++counts.rebound;
            else
                ++counts.left;
        }
    };
    table(d.rela, d.relaSize);
    if (d.pltIsRela)
        table(d.plt, d.pltSize);
}

// Priority 101: before the shim's other set-up and before Live's own code.
__attribute__((constructor(101))) void hideLiveExports()
{
    const char *off = getenv("LIVE_HIDE_EXPORTS");
    if ((off && !strcmp(off, "0")) || liveBuild() == LiveBuild::Unknown)
        return;
    std::vector<Object> objects;
    dl_iterate_phdr(collect, &objects);
    if (objects.empty())
        return;
    if (!(off && !strcmp(off, "fonts")))
        collectOpenSSLNames(objects);
    std::vector<uintptr_t> hidden;
    const int count = hideInMain(objects.front(), hidden);
    if (!count) {
        menufixTrace("hideexports: nothing hidden\n");
        return;
    }
    Counts counts;
    for (size_t i = 1; i < objects.size(); ++i)
        rebind(objects[i], hidden, false, counts);
    Counts check; // what still points into Live afterwards
    for (size_t i = 1; i < objects.size(); ++i)
        rebind(objects[i], hidden, true, check);
    menufixTrace("hideexports: %d hidden, %d bound again, %d left\n", count, counts.rebound, check.left);
    menufixTrace("hideexports: %d OpenSSL names\n", int(opensslNames().size()));
}

// The Max runtime (libMaxPlugLib.so, loaded with the first Max device or the
// browser's Max for Live previews, RTLD_GLOBAL | RTLD_DEEPBIND) carries its own
// libcurl 7.79.1 and OpenSSL 1.1.1j and exports them too. A plug-in loaded after
// it that uses the system's libcurl (Vital's sign-in: JUCE dlopen()s libcurl)
// had libcurl's calls to its own functions (curl_strequal) and to OpenSSL
// resolved into Max's copies: Live crashed in the sign-in (SIGSEGV in
// curl_strequal, libMaxPlugLib.so, from the system's curl_multi_perform).
// When the runtime appears, the same is done to it as to Live above, for its
// curl_* functions and the OpenSSL names: first its own references to them are
// bound to its own copies (what RTLD_DEEPBIND gives it: they are lazily bound,
// and the loader would skip them once they are local), then they are marked
// local, then slots of other libraries already bound to them are bound again.
// No other file of Live or Max uses them by name.
bool maxName(const char *s)
{
    return !strncmp(s, "curl_", 5) || opensslNames().count(s);
}

int hideInMax(const Object &max, std::vector<uintptr_t> &hidden)
{
    const Dynamic d = readDynamic(max.base, dynamicOf(max));
    const uint32_t count = symbolCount(d);
    if (!d.sym || !d.str || !count)
        return 0;
    auto mine = [&](size_t i) {
        const ElfW(Sym) &s = d.sym[i];
        const unsigned bind = ELF64_ST_BIND(s.st_info);
        return s.st_shndx != SHN_UNDEF && ELF64_ST_TYPE(s.st_info) == STT_FUNC && (bind == STB_GLOBAL || bind == STB_WEAK)
               && maxName(d.str + s.st_name);
    };
    // Its own references first.
    int bound = 0;
    auto table = [&](const ElfW(Rela) *r, size_t bytes) {
        for (size_t i = 0; r && i < bytes / sizeof(ElfW(Rela)); ++i) {
            const unsigned type = ELF64_R_TYPE(r[i].r_info);
            const size_t index = ELF64_R_SYM(r[i].r_info);
            if ((type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT && type != R_X86_64_64) || !index || !mine(index))
                continue;
            auto *slot = reinterpret_cast<uintptr_t *>(max.base + r[i].r_offset);
            const uintptr_t to = max.base + d.sym[index].st_value + (type == R_X86_64_64 ? uintptr_t(r[i].r_addend) : 0);
            const uintptr_t at = reinterpret_cast<uintptr_t>(slot);
            if (*slot == to || withWritable(max, at, at + sizeof *slot, [&] { *slot = to; }))
                ++bound;
        }
    };
    table(d.rela, d.relaSize);
    if (d.pltIsRela)
        table(d.plt, d.pltSize);
    std::vector<uint32_t> found;
    for (uint32_t i = 1; i < count; ++i)
        if (mine(i))
            found.push_back(i);
    if (found.empty())
        return 0;
    const uintptr_t start = reinterpret_cast<uintptr_t>(d.sym + found.front()),
                    end = reinterpret_cast<uintptr_t>(d.sym + found.back() + 1);
    if (!withWritable(max, start, end, [&] {
            for (uint32_t i : found)
                d.sym[i].st_info = ELF64_ST_INFO(STB_LOCAL, ELF64_ST_TYPE(d.sym[i].st_info));
        }))
        return 0;
    for (uint32_t i : found)
        hidden.push_back(max.base + d.sym[i].st_value);
    std::sort(hidden.begin(), hidden.end());
    menufixTrace("hideexports: Max runtime: %d of its own references bound to it\n", bound);
    return int(found.size());
}

// True once the runtime was there and has been handled.
bool hideMaxExports()
{
    std::vector<Object> objects;
    dl_iterate_phdr(collect, &objects);
    size_t max = objects.size();
    for (size_t i = 1; i < objects.size(); ++i) {
        const char *slash = strrchr(objects[i].name, '/');
        if (slash && !strcmp(slash + 1, "libMaxPlugLib.so"))
            max = i;
    }
    if (max == objects.size())
        return false;
    std::vector<uintptr_t> hidden;
    const int count = hideInMax(objects[max], hidden);
    Counts counts, check;
    for (size_t i = 0; count && i < objects.size(); ++i)
        if (i != max)
            rebind(objects[i], hidden, false, counts, maxName);
    for (size_t i = 0; count && i < objects.size(); ++i)
        if (i != max)
            rebind(objects[i], hidden, true, check, maxName);
    menufixTrace("hideexports: Max runtime: %d hidden, %d bound again, %d left\n", count, counts.rebound, check.left);
    return true;
}

} // namespace

// From the shim's start-up (menufix.cpp): checks every 200 ms for the Max runtime.
void installHideMaxExports()
{
    const char *off = getenv("LIVE_HIDE_EXPORTS");
    if ((off && (!strcmp(off, "0") || !strcmp(off, "fonts"))) || liveBuild() == LiveBuild::Unknown)
        return;
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, timer, [timer] {
        if (hideMaxExports())
            timer->deleteLater();
    });
    timer->start(200);
}
