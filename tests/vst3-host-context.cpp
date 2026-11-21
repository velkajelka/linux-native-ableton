// Checks for the host context shim/vst3runloop.cpp gives each VST3 plug-in
// factory, run by
// tests/vst3-host-context.sh. On Linux a plug-in such as Serum 2 takes the
// host's Steinberg::Linux::IRunLoop from the context passed to
// IPluginFactory3::setHostContext(). On Live 12.4.7 the shim's
// vst3CopyFactoryPointer keeps a newly loaded factory and hands it a context
// answering IRunLoop. Expected values are literals or come from the fake factory's own
// counters, never from the code under test.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

bool installFactoryHostContext(uintptr_t base); // shim/vst3runloop.cpp

void *vst3CopyFactoryPointer(void **dst, void *const *src); // shim/vst3runloop.cpp
void *qtRunLoop();                                           // shim/vst3runloop.cpp

// What Live's own function does today: copy and addRef, no host context. The
// shim's (strong) definition replaces this weak one, so until the shim has
// it the checks run against Live's behaviour and fail.
struct LiveAddRef {
    virtual int32_t queryInterface(const char *, void **) = 0;
    virtual uint32_t addRef() = 0;
};
__attribute__((weak)) void *vst3CopyFactoryPointer(void **dst, void *const *src)
{
    *dst = *src;
    if (*dst)
        static_cast<LiveAddRef *>(*dst)->addRef();
    return dst;
}

// Link stubs for what vst3runloop.cpp uses from the rest of the shim.
void menufixTrace(const char *, int, int, int) {}
void pluginViewCreated(void *) {}
#include "livebuild.h"
LiveBuild liveBuild() { return LiveBuild::Unknown; }

namespace {

using tresult = int32_t;
constexpr tresult kResultOk = 0;
constexpr tresult kNoInterface = -1;

// Non-COM (Linux) FUID byte order.
constexpr unsigned char kFUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr unsigned char kIPluginFactory[16] = {0x7A, 0x4D, 0x81, 0x1C, 0x52, 0x11, 0x4A, 0x1F,
                                               0xAE, 0xD9, 0xD2, 0xEE, 0x0B, 0x43, 0xBF, 0x9F};
constexpr unsigned char kIPluginFactory2[16] = {0x00, 0x07, 0xB6, 0x50, 0xF2, 0x4B, 0x4C, 0x0B,
                                                0xA4, 0x64, 0xED, 0xB9, 0xF0, 0x0B, 0x2A, 0xBB};
constexpr unsigned char kIPluginFactory3[16] = {0x45, 0x55, 0xA2, 0xAB, 0xC1, 0x23, 0x4E, 0x57,
                                                0x9B, 0x12, 0x29, 0x10, 0x36, 0x87, 0x89, 0x31};
constexpr unsigned char kIRunLoop[16] = {0x18, 0xC3, 0x53, 0x66, 0x97, 0x76, 0x4F, 0x1A,
                                         0x9C, 0x5B, 0x83, 0x85, 0x7A, 0x87, 0x13, 0x89};

struct FUnknown {
    virtual tresult queryInterface(const char *iid, void **obj) = 0;
    virtual uint32_t addRef() = 0;
    virtual uint32_t release() = 0;
};

// IPluginFactory3's vtable order: setHostContext is slot 9 (offset 0x48).
struct FakeFactory : FUnknown {
    bool version3;
    int refs = 1;
    int hostContextCalls = 0;
    FUnknown *context = nullptr;
    tresult hostContextResult = kResultOk;
    explicit FakeFactory(bool v3) : version3(v3) {}

    tresult queryInterface(const char *iid, void **obj) override
    {
        const bool known = !memcmp(iid, kFUnknown, 16) || !memcmp(iid, kIPluginFactory, 16)
            || (version3 && (!memcmp(iid, kIPluginFactory2, 16) || !memcmp(iid, kIPluginFactory3, 16)));
        if (!known) {
            *obj = nullptr;
            return kNoInterface;
        }
        ++refs;
        *obj = this;
        return kResultOk;
    }
    uint32_t addRef() override { return ++refs; }
    uint32_t release() override { return --refs; }
    virtual tresult getFactoryInfo(void *) { return kResultOk; }
    virtual int32_t countClasses() { return 0; }
    virtual tresult getClassInfo(int32_t, void *) { return kNoInterface; }
    virtual tresult createInstance(const char *, const char *, void **) { return kNoInterface; }
    virtual tresult getClassInfo2(int32_t, void *) { return kNoInterface; }
    virtual tresult getClassInfoUnicode(int32_t, void *) { return kNoInterface; }
    virtual tresult setHostContext(FUnknown *c)
    {
        ++hostContextCalls;
        context = c;
        return hostContextResult;
    }
};

int failures;

void check(const char *what, bool ok)
{
    printf("%s %s\n", ok ? "  ok  " : "FAIL:", what);
    if (!ok)
        ++failures;
}

void factory3()
{
    FakeFactory f(true);
    void *src = static_cast<FUnknown *>(&f);
    void *dst = nullptr;
    void *r = vst3CopyFactoryPointer(&dst, &src);
    check("IPluginFactory3: the pointer is copied and dst returned", dst == src && r == &dst);
    check("IPluginFactory3: the copy holds one reference (no leak, no extra release)", f.refs == 2);
    check("IPluginFactory3: setHostContext is called once", f.hostContextCalls == 1);
    void *loop = nullptr;
    const bool answers = f.context
        && f.context->queryInterface(reinterpret_cast<const char *>(kIRunLoop), &loop) == kResultOk;
    check("the host context answers Steinberg::Linux::IRunLoop", answers);
    check("the IRunLoop it gives is the shim's Qt run loop", answers && loop == qtRunLoop());
    void *unknown = nullptr;
    check("the host context answers FUnknown",
          f.context && f.context->queryInterface(reinterpret_cast<const char *>(kFUnknown), &unknown) == kResultOk
              && unknown);
}

void factory3Refuses()
{
    FakeFactory f(true);
    f.hostContextResult = 1; // kResultFalse: a factory that wants no context
    void *src = static_cast<FUnknown *>(&f);
    void *dst = nullptr;
    vst3CopyFactoryPointer(&dst, &src);
    check("a factory refusing the context is still copied with one reference", dst == src && f.refs == 2);
}

void factory1()
{
    FakeFactory f(false);
    void *src = static_cast<FUnknown *>(&f);
    void *dst = nullptr;
    vst3CopyFactoryPointer(&dst, &src);
    check("IPluginFactory only: the pointer is copied with one reference", dst == src && f.refs == 2);
    check("IPluginFactory only: no setHostContext call", f.hostContextCalls == 0);
}

void nullFactory()
{
    void *src = nullptr;
    void *dst = reinterpret_cast<void *>(uintptr_t(1));
    void *r = vst3CopyFactoryPointer(&dst, &src);
    check("a null factory copies as null", dst == nullptr && r == &dst);
}

// The redirect itself, against a stand-in code image: anonymous memory with
// the expected 21 bytes (or one wrong byte) at the hooked address, mapped
// read + execute.
constexpr uintptr_t kFactoryCopy = 0x5a032d0;
constexpr unsigned char kLiveCopy[21] = {0x48, 0x89, 0xf8, 0x48, 0x8b, 0x3e, 0x48, 0x89, 0x38, 0x48, 0x85,
                                         0xff, 0x74, 0x06, 0x48, 0x8b, 0x07, 0xff, 0x60, 0x08, 0xc3};

uintptr_t codeImage(unsigned char lastByte, size_t *size)
{
    const uintptr_t page = sysconf(_SC_PAGESIZE);
    *size = (kFactoryCopy + sizeof kLiveCopy + page - 1) & ~(page - 1);
    void *mem = mmap(nullptr, *size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED)
        return 0;
    const uintptr_t base = uintptr_t(mem);
    memcpy(reinterpret_cast<void *>(base + kFactoryCopy), kLiveCopy, sizeof kLiveCopy);
    reinterpret_cast<unsigned char *>(base + kFactoryCopy)[20] = lastByte;
    mprotect(reinterpret_cast<void *>((base + kFactoryCopy) & ~(page - 1)), page, PROT_READ | PROT_EXEC);
    return base;
}

void factoryHook()
{
    size_t size = 0;
    const uintptr_t base = codeImage(0xc3, &size);
    if (!base) {
        check("stand-in code image mapped", false);
        return;
    }
    check("matching bytes: the redirect is installed", installFactoryHostContext(base));
    FakeFactory f(true);
    void *src = static_cast<FUnknown *>(&f);
    void *dst = nullptr;
    using CopyFn = void *(*)(void **, void *const *);
    reinterpret_cast<CopyFn>(base + kFactoryCopy)(&dst, &src);
    check("a call through Live's copy reaches the factory: copied, one reference, one setHostContext",
          dst == src && f.refs == 2 && f.hostContextCalls == 1);
    munmap(reinterpret_cast<void *>(base), size);

    const uintptr_t other = codeImage(0xcc, &size);
    if (!other) {
        check("stand-in code image mapped", false);
        return;
    }
    check("one byte different: no redirect", !installFactoryHostContext(other));
    unsigned char expected[21];
    memcpy(expected, kLiveCopy, sizeof expected);
    expected[20] = 0xcc;
    check("one byte different: the code is left as it was",
          !memcmp(reinterpret_cast<void *>(other + kFactoryCopy), expected, sizeof expected));
    munmap(reinterpret_cast<void *>(other), size);
}

} // namespace

int main()
{
    factory3();
    factory3Refuses();
    factory1();
    nullFactory();
    factoryHook();
    if (failures)
        printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
