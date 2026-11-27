// Checks for how "Show in Folder" (shim/showinfolder.cpp) is installed, run by
// tests/show-in-folder.sh:
//   - on a stand-in code image with both expected first bytes in place, both
//     functions are replaced;
//   - with either function's first bytes different, neither function's bytes
//     change, and the trace says nothing was replaced;
//   - on a program that is not Live 12.4.7 (this test program), the trace says
//     the build was skipped.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>

bool installShowInFolderAt(uintptr_t base);
void installShowInFolder();

namespace {

std::string trace;
int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// The two hooked addresses in Live 12.4.7, and the bytes expected there.
constexpr uintptr_t kCheckAt = 0x1bf0c50;
constexpr uintptr_t kLaunchAt = 0x1b37850;
const unsigned char kCheck[] = {0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x58, 0xe8, 0xc4, 0x65, 0xf3, 0xff};
const unsigned char kLaunch[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                 0x53, 0x48, 0x81, 0xec, 0xb8, 0x02, 0x00, 0x00};
constexpr size_t kImage = kCheckAt + 64;

// A read-and-execute image holding the two prologues; `breakCheck` or
// `breakLaunch` changes one byte of that prologue.
unsigned char *image(bool breakCheck, bool breakLaunch)
{
    void *m = mmap(nullptr, kImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return nullptr;
    auto *b = static_cast<unsigned char *>(m);
    memcpy(b + kCheckAt, kCheck, sizeof kCheck);
    memcpy(b + kLaunchAt, kLaunch, sizeof kLaunch);
    if (breakCheck)
        b[kCheckAt + 8] ^= 0xff;
    if (breakLaunch)
        b[kLaunchAt + 8] ^= 0xff;
    mprotect(m, kImage, PROT_READ | PROT_EXEC);
    return b;
}

void mismatch(bool breakCheck, bool breakLaunch, const char *what)
{
    unsigned char *b = image(breakCheck, breakLaunch);
    if (!b) {
        check(false, "stand-in image mapped");
        return;
    }
    unsigned char checkBefore[sizeof kCheck], launchBefore[sizeof kLaunch];
    memcpy(checkBefore, b + kCheckAt, sizeof checkBefore);
    memcpy(launchBefore, b + kLaunchAt, sizeof launchBefore);
    trace.clear();
    const bool done = installShowInFolderAt(reinterpret_cast<uintptr_t>(b));
    std::printf("  (%s) trace: %s", what, trace.c_str());
    check(!done, "installer reports nothing replaced");
    check(memcmp(b + kCheckAt, checkBefore, sizeof checkBefore) == 0, "availability function's bytes unchanged");
    check(memcmp(b + kLaunchAt, launchBefore, sizeof launchBefore) == 0, "launch function's bytes unchanged");
    check(trace.find("launch 0, check 0") != std::string::npos, "trace: launch 0, check 0");
    munmap(b, kImage);
}

} // namespace

void menufixTrace(const char *fmt, int a, int b, int c)
{
    char line[256];
    std::snprintf(line, sizeof line, fmt, a, b, c);
    trace += line;
}

int main()
{
    std::printf("stand-in image, both first bytes as expected:\n");
    if (unsigned char *b = image(false, false)) {
        trace.clear();
        const bool done = installShowInFolderAt(reinterpret_cast<uintptr_t>(b));
        check(done, "installer reports both replaced");
        check(memcmp(b + kCheckAt, kCheck, sizeof kCheck) != 0, "availability function replaced");
        check(memcmp(b + kLaunchAt, kLaunch, sizeof kLaunch) != 0, "launch function replaced");
        check(trace.find("launch 1, check 1") != std::string::npos, "trace: launch 1, check 1");
        munmap(b, kImage);
    } else {
        check(false, "stand-in image mapped");
    }
    std::printf("availability function's first bytes differ:\n");
    mismatch(true, false, "check differs");
    std::printf("launch function's first bytes differ:\n");
    mismatch(false, true, "launch differs");

    std::printf("not Live 12.4.7:\n");
    trace.clear();
    installShowInFolder();
    std::printf("  trace: %s", trace.empty() ? "(none)\n" : trace.c_str());
    check(trace.find("showinfolder") != std::string::npos && trace.find("not") != std::string::npos,
          "trace says this build is skipped");
    return failures ? 1 : 0;
}
