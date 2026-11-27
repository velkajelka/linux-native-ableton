// Desktop file dialogs for the Push 3 Live build.
//
// Live has three file-dialog entry points per platform (UiWidgets FileDialogs):
// open, save and choose-folder. Windows Live installs native IFileDialog
// versions at startup (DialogOsL_win.cpp, FUN_1431aec30); the Linux build
// installs Live's own drawn dialogs (0x22e5c23):
//   open   0x217ec70 (TText const &title, path const &dir, int kind,
//                     Receiver *receiver, ...)
//   save   0x217fbc0 (TText const &title, TText const &name, path const &dir,
//                     FileTypeMask mask, Receiver *receiver) -> int
//   folder 0x217f3d0 (path *result, TText const &title, path const &dir,
//                     bool) -> path
// All three run their dialog modally (0x217d930). On OK, open and save hand
// the path to the receiver (vtable +0x98); save then reports the file type
// (+0xa0, type from 0x1b387d0) and returns +0xa8. A cancelled dialog throws
// Live's silent exception, which its callers catch (all three do; without
// it Live goes on with an empty path: 'The file "" could not be opened').
//
// Live calls them through its dialog table, filled at startup (0x22e5c10):
// open and save are function pointers set through 0x1da6e70 / 0x1da6e80
// (stored at 0x9c08128 / 0x9c08130), choose-folder a std::function set
// through 0x205f620 (stored at 0x9c12e18), built from Live's own manager and
// invoker for a plain function pointer (0x13618e0 / 0x13618c0). This file
// puts QFileDialog versions into that table through the same setters, once
// Live has registered its own, as Windows Live puts its IFileDialog versions
// there; Live's code is not changed. With
// QT_QPA_PLATFORMTHEME=xdgdesktopportal (standalone.cpp's default) the desktop's
// own dialog is shown through xdg-desktop-portal's FileChooser: KDE's on Plasma,
// GTK's on GNOME. The shim makes that request itself (GDBus; portalRequest)
// rather than through Qt 5.15's portal dialog, which takes a failed request
// (response 2, or an error reply) for a cancelled one: on niri, whose default
// portal configuration names GNOME's portal first, GNOME's file chooser fails
// unless Nautilus is installed, and every open, save and folder dialog did
// nothing. After a failed request, and where no portal offers a FileChooser at
// all (a bare window manager), Qt's own dialog is used. LIVE_NATIVE_DIALOGS=0
// keeps Live's dialogs.
//
// Live keeps running while its own dialog is open. Its modal loop (Live 12
// 0x1ff39b0, Live 11 0x1b9a370) starts two timers next to its QEventLoop:
// every 10 ms one calls a slot (0x1ff9770 / 0x1b95bb0), every 50 ms the other
// (0x1ff9810 / 0x1b95c50). For the time of the loop it sets the flag
// 0x9bdfc79 / 0x81688d1 (scoped, 0x1b26c00 / 0x162d430); the 10 ms tick does
// nothing without it. Live's regular tick is the frame the dialog was opened
// from, so a QFileDialog's loop alone left Live's display standing still until
// the dialog closed (playback went on). LiveTicks does the same around these
// dialogs: the flag, then the two timers.
//
// A path is a std::string (UTF-8). File kinds map to a 128-bit mask of file
// type ids (0x1be0b80); the ids, names and extensions below are Live's own
// file type table (registered in 0x1be15e0). Addresses are for Live 12.4.7.
#include "livebuild.h"
#include <QApplication>
#include <QGuiApplication>
#include <QTimer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QDialog>
#include <QUrl>
#include <QWidget>
#include <cstdint>
#include <cstring>
#include <cxxabi.h>
#include <iterator>
#include <link.h>
#include <string>
#include <unistd.h>
#pragma push_macro("signals")
#undef signals // a member name in GLib's headers, a keyword for Qt
#include <gio/gio.h>
#pragma pop_macro("signals")
#include <new>
#include <string>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

extern "C" void *__cxa_allocate_exception(size_t) noexcept;
extern "C" void __cxa_throw(void *, void *, void (*)(void *));

namespace {

// Live 12.4.7 / Live 11.3.43 (same signatures and receiver slots; Live 11
// registers its dialogs at 0x1e4bda9, file types at 0x170d300).
uintptr_t kOpen, kSave, kFolder;
uintptr_t kKindMask;            // file kind -> file type mask
uintptr_t kTypeOfPath;          // path -> file type id
uintptr_t kSilentException;     // constructor, 0x30 bytes
uintptr_t kSilentExceptionType; // typeinfo for TException
uintptr_t kSilentExceptionDtor;

// The dialog table (see the top of the file).
uintptr_t kSetOpen, kSetSave, kSetFolder;
uintptr_t kOpenSlot, kSaveSlot, kFolderSlot;
uintptr_t kPointerManager, kPointerInvoker;

bool setAddresses()
{
    if (!liveAddress(1, 1))
        return false;
    kOpen = liveAddress(0x217ec70, 0x1d073b0);
    kSave = liveAddress(0x217fbc0, 0x1d085f0);
    kFolder = liveAddress(0x217f3d0, 0x1d07c80);
    kKindMask = liveAddress(0x1be0b80, 0x170cb10);
    kTypeOfPath = liveAddress(0x1b387d0, 0x16447e0);
    kSilentException = liveAddress(0x1ba8690, 0x16d1a40);
    kSilentExceptionType = liveAddress(0x9436bd0, 0x7b755e8);
    kSilentExceptionDtor = liveAddress(0x1ba8900, 0x16d36c0);
    kSetOpen = liveAddress(0x1da6e70, 0x197fc00);
    kSetSave = liveAddress(0x1da6e80, 0x197fc10);
    kSetFolder = liveAddress(0x205f620, 0x1bf37d0);
    kOpenSlot = liveAddress(0x9c08128, 0x818f620);
    kSaveSlot = liveAddress(0x9c08130, 0x818f628);
    kFolderSlot = liveAddress(0x9c12e18, 0x8198a20);
    kPointerManager = liveAddress(0x13618e0, 0xf98ac0);
    kPointerInvoker = liveAddress(0x13618c0, 0xf98aa0);
    return true;
}

// libstdc++'s std::function: the stored callable, its manager, its invoker.
struct LiveFunction {
    void *callable, *unused;
    void (*manager)(LiveFunction *, const LiveFunction *, int);
    void *invoker;
};

struct FileType {
    int id;
    const char *name;
    const char *patterns;
};

// Live 12.4.7's file types with extensions (id, name, patterns).
const FileType kFileTypes12[] = {
    {2, "Macintosh AIFF", "*.aif *.aiff"},
    {3, "Ableton Meta Sound", "*.ams"},
    {70, "Broadcast Wave", "*.bwf *.rf64"},
    {49, "Encrypted FLAC", "*.eflac"},
    {42, "EXS", "*.exs"},
    {34, "Lossless Audio", "*.flac"},
    {41, "GigaSampler", "*.gig"},
    {35, "MPEG 4 Audio", "*.m4a"},
    {5, "Standard MIDI File", "*.mid *.midi"},
    {28, "MPEG Audio Layer-3", "*.mp3"},
    {29, "Wave Simulation Text", "*.mp7"},
    {45, "NI Kontakt", "*.nki"},
    {33, "Ogg Vorbis", "*.ogg"},
    {36, "Propellerheads Rex", "*.rex *.rcy"},
    {37, "Propellerheads Rex", "*.rx2"},
    {4, "Sounddesigner II", "*.sd2"},
    {40, "SoundFont", "*.sf *.sf2"},
    {1, "Windows Wave", "*.wav"},
    {55, "WavPack", "*.wv"},
    {61, "Windows Media Audio", "*.wma"},
    {47, "Audio/Video", "*.avi"},
    {48, "Digital Video", "*.dv"},
    {50, "MPEG 4 Audio/Video", "*.mp4 *.m4v"},
    {46, "QuickTime Movie", "*.mov"},
    {62, "MPEG Media Video", "*.mpg *.m2v"},
    {60, "Windows Media Video", "*.wmv"},
    {44, "ISO Image", "*.iso"},
    {43, "Toast Image", "*.toast"},
    {7, "Bmp", "*.bmp"},
    {51, "JPEG graphics", "*.jpeg *.jpg"},
    {6, "Pcx", "*.pcx"},
    {57, "Portable Network Graphics", "*.png"},
    {8, "Tiff", "*.tif *.tiff"},
    {12, "Ableton Binary Live Set", "*.abl"},
    {17, "Ableton Device", "*.adv"},
    {18, "Ableton Device Group", "*.adg"},
    {16, "Ableton Groove", "*.agr"},
    {15, "Ableton Live Clip", "*.alc"},
    {11, "Ableton Live Set", "*.als"},
    {52, "Ableton Max Device", "*.amxd *.amp"},
    {25, "VST Bank", "*.fxb"},
    {26, "VST Preset", "*.fxp"},
    {71, "Scala Tuning", "*.ascl *.scl"},
    {53, "Ableton Instrument Rack Archive", "*.auz"},
    {9, "Ableton Language", "*.lng"},
    {20, "Ableton Log", "*.log"},
    {19, "Ableton Sample Data", "*.asd"},
    {64, "Ableton Push Cache", "*.appc"},
    {10, "Ableton Config", "*.cfg"},
    {23, "Ableton Text", "*.txt"},
    {22, "Ableton Skin", "*.ask"},
    {38, "XML", "*.xml"},
    {68, "Metadata File", "*.xmp"},
    {73, "Ableton Live Bundle", "*.ablx"},
    {21, "Executable", "*.exe *.com"},
    {63, "Ableton Analysis Data", "*.aad"},
    {67, "Band", "*.band"},
    {31, "Database", "*.db"},
    {54, "GNU Zip", "*.gz"},
    {30, "Index", "*.idx"},
    {39, "Ini", "*.ini"},
    {66, "JSON", "*.json"},
    {59, "Lock", "*.lock"},
    {32, "Ableton Live Pack", "*.alp"},
    {24, "Python", "*.py"},
    {56, "Ableton Remote Data", "*.ard"},
    {58, "Crash Dump", "*.dmp"},
    {65, "ZIP File", "*.zip"},
};

// Live 11.3.43's (other ids; from its registrations at 0x170d300).
const FileType kFileTypes11[] = {
    {2, "Macintosh AIFF", "*.aif *.aiff"},
    {3, "Ableton Meta Sound", "*.ams"},
    {48, "Encrypted FLAC", "*.eflac"},
    {41, "EXS", "*.exs"},
    {33, "Lossless Audio", "*.flac"},
    {40, "GigaSampler", "*.gig"},
    {34, "MPEG 4 Audio", "*.m4a"},
    {5, "Standard MIDI File", "*.mid *.midi"},
    {27, "MPEG Audio Layer-3", "*.mp3"},
    {28, "Wave Simulation Text", "*.mp7"},
    {44, "NI Kontakt", "*.nki"},
    {32, "Ogg Vorbis", "*.ogg"},
    {35, "Propellerheads Rex", "*.rex *.rcy"},
    {36, "Propellerheads Rex", "*.rx2"},
    {4, "Sounddesigner II", "*.sd2"},
    {39, "SoundFont", "*.sf *.sf2"},
    {1, "Windows Wave", "*.wav"},
    {54, "WavPack", "*.wv"},
    {60, "Windows Media Audio", "*.wma"},
    {46, "Audio/Video", "*.avi"},
    {47, "Digital Video", "*.dv"},
    {49, "MPEG 4 Audio/Video", "*.mp4 *.m4v"},
    {45, "QuickTime Movie", "*.mov"},
    {61, "MPEG Media Video", "*.mpg *.m2v"},
    {59, "Windows Media Video", "*.wmv"},
    {43, "ISO Image", "*.iso"},
    {42, "Toast Image", "*.toast"},
    {7, "Bmp", "*.bmp"},
    {50, "JPEG graphics", "*.jpeg *.jpg"},
    {6, "Pcx", "*.pcx"},
    {56, "Portable Network Graphics", "*.png"},
    {8, "Tiff", "*.tif *.tiff"},
    {12, "Ableton Live Set", "*.abl"},
    {13, "Ableton Live Set Bundle", "*.ablbundle"},
    {16, "Ableton Device", "*.adv"},
    {17, "Ableton Device Group", "*.adg"},
    {15, "Ableton Groove", "*.agr"},
    {14, "Ableton Live Clip", "*.alc"},
    {11, "Ableton Live Set", "*.als"},
    {51, "Ableton Max Device", "*.amxd *.amp"},
    {26, "Audio Unit Preset", "*.aupreset"},
    {68, "VST3 Preset File", "*.vstpreset"},
    {24, "VST Bank", "*.fxb"},
    {25, "VST Program", "*.fxp"},
    {52, "Ableton Authorization File", "*.auz"},
    {9, "Ableton Language File", "*.lng"},
    {19, "Log File", "*.log"},
    {18, "Ableton Sound Data", "*.asd"},
    {63, "Plugin Parameter Config", "*.appc"},
    {10, "Ableton Preferences", "*.cfg"},
    {22, "Ableton Text", "*.txt"},
    {21, "Ableton Theme", "*.ask"},
    {37, "XML", "*.xml"},
    {67, "Metadata File", "*.xmp"},
    {20, "Application", "*.exe *.com"},
    {62, "Ableton Audio Dump", "*.aad"},
    {66, "Band File", "*.band"},
    {30, "Finder Index Database", "*.db"},
    {53, "GNU Zip", "*.gz"},
    {29, "Cache Index File", "*.idx"},
    {38, "Windows Config", "*.ini"},
    {65, "JSON File", "*.json"},
    {58, "Lock File", "*.lock"},
    {31, "Ableton Live Pack", "*.alp"},
    {23, "Python Script", "*.py"},
    {55, "View Render Dumps", "*.ard"},
    {57, "Crash Log", "*.dmp"},
    {64, "ZIP File", "*.zip"},
};

struct FileTypes {
    const FileType *begin_, *end_;
    const FileType *begin() const { return begin_; }
    const FileType *end() const { return end_; }
};
FileTypes fileTypes()
{
    if (liveBuild() == LiveBuild::Live11_3_43)
        return {std::begin(kFileTypes11), std::end(kFileTypes11)};
    return {std::begin(kFileTypes12), std::end(kFileTypes12)};
}

struct TypeMask {
    uint64_t lo, hi;
    bool has(int id) const { return id < 64 ? (lo >> id) & 1 : id < 128 && (hi >> (id - 64)) & 1; }
};

// Live's file receiver: +0x98 set path, +0xa0 set type, +0xa8 result.
using SetPathFn = void (*)(void *, const std::string *);
using SetTypeFn = void (*)(void *, int, bool);
using ResultFn = int (*)(void *);

uintptr_t base;

// Live's modal-loop timer slots and flag (see the top of the file): the slots
// are QSlotObjectBase impl functions of lambdas without captures, called as Qt
// does, impl(Call = 1, slot object, receiver, arguments, ret). The setter is
// Live's own "flag = 1" function, checked to find the flag where expected.
struct ModalCode {
    uintptr_t at;
    const unsigned char *bytes;
    size_t size;
};
const unsigned char kTickFast12[] = {0x83, 0xff, 0x01};                    // cmp $1,%edi
const unsigned char kTickSlow12[] = {0x50, 0x83, 0xff, 0x01};              // push %rax; cmp $1,%edi
const unsigned char kSetFlag12[] = {0xc6, 0x05, 0x82, 0x90, 0x0b, 0x08, 0x01, 0xc3}; // movb $1,0x9bdfc79; ret
const unsigned char kTickFast11[] = {0x53, 0x48, 0x83, 0xec, 0x30, 0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0};
const unsigned char kTickSlow11[] = {0x53, 0x48, 0x83, 0xec, 0x10, 0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0};
const unsigned char kSetFlag11[] = {0xc6, 0x05, 0xaa, 0xb4, 0xb3, 0x06, 0x01, 0xc3}; // movb $1,0x81688d1; ret

class LiveTicks {
public:
    LiveTicks()
    {
        const bool live11 = liveBuild() == LiveBuild::Live11_3_43;
        const ModalCode fast = live11 ? ModalCode{0x1b95bb0, kTickFast11, sizeof kTickFast11}
                                      : ModalCode{0x1ff9770, kTickFast12, sizeof kTickFast12};
        const ModalCode slow = live11 ? ModalCode{0x1b95c50, kTickSlow11, sizeof kTickSlow11}
                                      : ModalCode{0x1ff9810, kTickSlow12, sizeof kTickSlow12};
        const ModalCode setFlag = live11 ? ModalCode{0x162d420, kSetFlag11, sizeof kSetFlag11}
                                         : ModalCode{0x1b26bf0, kSetFlag12, sizeof kSetFlag12};
        const uintptr_t flag = live11 ? 0x81688d1 : 0x9bdfc79;
        auto matches = [](const ModalCode &c) { return !memcmp(reinterpret_cast<const void *>(base + c.at), c.bytes, c.size); };
        static const bool known = liveAddress(1, 1) && matches(fast) && matches(slow) && matches(setFlag);
        if (!known) {
            menufixTrace("filedialogs: Live's modal timers not as expected, off\n");
            return;
        }
        flag_ = reinterpret_cast<volatile bool *>(base + flag);
        saved_ = *flag_;
        *flag_ = true;
        start(fast_, fast.at, 10);
        start(slow_, slow.at, 50);
    }
    ~LiveTicks()
    {
        fast_.stop();
        slow_.stop();
        if (flag_)
            *flag_ = saved_;
    }
    LiveTicks(const LiveTicks &) = delete;
    LiveTicks &operator=(const LiveTicks &) = delete;

private:
    static void start(QTimer &timer, uintptr_t impl, int ms)
    {
        QObject::connect(&timer, &QTimer::timeout, [impl] {
            alignas(16) unsigned char slotObject[0x18] = {}; // what the lambdas would own: nothing
            reinterpret_cast<void (*)(int, void *, QObject *, void **, bool *)>(base + impl)(1, slotObject, nullptr,
                                                                                            nullptr, nullptr);
        });
        timer.start(ms);
    }
    QTimer fast_, slow_;
    volatile bool *flag_ = nullptr;
    bool saved_ = false;
};

template <typename Fn>
Fn virtualAt(void *object, size_t offset)
{
    return *reinterpret_cast<Fn *>(*static_cast<uintptr_t *>(object) + offset);
}

// TText: pointer to a refcounted block {int refs; int bytes incl. NUL; ...;
// UTF-16 at +0x10}.
QString fromTText(const void *text)
{
    const auto block = text ? *static_cast<const uintptr_t *>(text) : 0;
    if (!block)
        return {};
    const int units = *reinterpret_cast<const int *>(block + 4) / 2 - 1;
    return units > 0 ? QString::fromUtf16(reinterpret_cast<const char16_t *>(block + 0x10), units) : QString();
}

QString fromPath(const std::string *path)
{
    return path ? QString::fromStdString(*path) : QString();
}

// "Ableton Live Set (*.als)" entries for the types in the mask; with more than
// one, an "All supported" entry first.
QStringList filtersFor(TypeMask mask, QStringList *allPatterns = nullptr)
{
    QStringList filters, patterns;
    for (const FileType &t : fileTypes()) {
        if (!mask.has(t.id))
            continue;
        filters << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(t.name), QString::fromLatin1(t.patterns));
        patterns << QString::fromLatin1(t.patterns).split(' ');
    }
    if (filters.size() > 1)
        filters.prepend(QStringLiteral("All supported files (%1)").arg(patterns.join(' ')));
    if (allPatterns)
        *allPatterns = patterns;
    return filters;
}

QWidget *dialogParent()
{
    if (QWidget *w = QApplication::activeWindow())
        return w;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->windowType() == Qt::Window)
            return w;
    return nullptr;
}

bool portalTheme()
{
    static const bool portal = qEnvironmentVariable("QT_QPA_PLATFORMTHEME") == QLatin1String("xdgdesktopportal");
    return portal;
}

// With the portal theme, Qt's QFileDialog would go through the portal again:
// it is only used as Qt's own dialog then.
QFileDialog::Options dialogOptions()
{
    return portalTheme() ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
}

bool portalFailed; // a request failed: Qt's own dialog for the rest of the session

// Whether to ask the portal: the theme is in use, a portal offers a FileChooser
// (checked once, at the first dialog) and no request has failed.
bool usePortal()
{
    static const bool offered = [] {
        if (!portalTheme())
            return false;
        QDBusMessage get = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        get << QStringLiteral("org.freedesktop.portal.FileChooser") << QStringLiteral("version");
        const bool ok = QDBusConnection::sessionBus().call(get, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
        menufixTrace("filedialogs: portal FileChooser %d\n", ok);
        return ok;
    }();
    return offered && !portalFailed;
}

// "Name (*.a *.b)" -> ("Name", [(0, "*.a"), (0, "*.b")]), the portal's filter.
GVariant *portalFilter(const QString &filter)
{
    const int open = filter.lastIndexOf('('), close = filter.lastIndexOf(')');
    const QString name = open > 0 ? filter.left(open).trimmed() : filter;
    GVariantBuilder patterns;
    g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
    if (open >= 0 && close > open)
        for (const QString &p : filter.mid(open + 1, close - open - 1).split(' ', Qt::SkipEmptyParts))
            g_variant_builder_add(&patterns, "(us)", 0u, p.toUtf8().constData());
    return g_variant_new("(s@a(us))", name.toUtf8().constData(), g_variant_builder_end(&patterns));
}

void addFilters(GVariantBuilder *options, const QStringList &filters, const QString &current)
{
    if (filters.isEmpty())
        return;
    GVariantBuilder list;
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(sa(us))"));
    for (const QString &f : filters)
        g_variant_builder_add_value(&list, portalFilter(f));
    g_variant_builder_add(options, "{sv}", "filters", g_variant_builder_end(&list));
    if (!current.isEmpty())
        g_variant_builder_add(options, "{sv}", "current_filter", portalFilter(current));
}

struct PortalAnswer {
    enum Status { Ok, Cancelled, Failed } status = Failed;
    QStringList paths;
    QString filter; // the name of the filter the dialog ended with
};

struct PortalRequest {
    GDBusConnection *bus = nullptr;
    std::string handle;
    guint subscription = 0;
    bool called = false, answered = false;
    guint32 response = 2;
    GVariant *results = nullptr;
};

void portalResponse(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *parameters,
                    gpointer data)
{
    auto *r = static_cast<PortalRequest *>(data);
    if (r->answered || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
        return;
    g_variant_get(parameters, "(u@a{sv})", &r->response, &r->results);
    r->answered = true;
}

void subscribe(PortalRequest *r)
{
    r->subscription = g_dbus_connection_signal_subscribe(r->bus, nullptr, "org.freedesktop.portal.Request", "Response",
                                                         r->handle.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                         portalResponse, r, nullptr);
}

void portalCalled(GObject *source, GAsyncResult *result, gpointer data)
{
    auto *r = static_cast<PortalRequest *>(data);
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    r->called = true;
    if (!reply) {
        menufixTrace("filedialogs: portal call failed: %d\n", error ? error->code : -1);
        g_clear_error(&error);
        r->answered = true; // response stays 2
        return;
    }
    const gchar *handle = nullptr;
    g_variant_get(reply, "(&o)", &handle);
    if (handle && r->handle != handle) { // portals before 0.9 choose the path themselves
        g_dbus_connection_signal_unsubscribe(r->bus, r->subscription);
        r->handle = handle;
        subscribe(r);
    }
    g_variant_unref(reply);
}

// One FileChooser request (OpenFile or SaveFile) with `options` (consumed),
// modal like Qt's own portal dialog: an application-modal QDialog that is
// never on screen blocks Live's windows until the portal answers. The reply and
// the Response signal arrive in a main context of their own, which a 10 ms
// timer runs, so this works with any Qt event dispatcher.
PortalAnswer portalRequest(const char *method, const QString &title, GVariantBuilder *options)
{
    PortalAnswer answer;
    GError *error = nullptr;
    PortalRequest r;
    r.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!r.bus) {
        g_clear_error(&error);
        g_variant_builder_clear(options);
        return answer;
    }
    static int requests;
    const std::string token = "ableton_live_" + std::to_string(getpid()) + "_" + std::to_string(++requests);
    std::string sender = g_dbus_connection_get_unique_name(r.bus) + 1; // ":1.42" -> "1_42"
    for (char &c : sender)
        if (c == '.')
            c = '_';
    r.handle = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(options, "{sv}", "modal", g_variant_new_boolean(TRUE));

    QWidget *parent = dialogParent();
    const QByteArray window = parent ? "x11:" + QByteArray::number(qulonglong(parent->winId()), 16) : QByteArray();
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    subscribe(&r);
    g_dbus_connection_call(r.bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.FileChooser", method,
                           g_variant_new("(ss@a{sv})", window.constData(), title.toUtf8().constData(),
                                         g_variant_builder_end(options)),
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, G_MAXINT, nullptr, portalCalled, &r);
    g_main_context_pop_thread_default(context);

    QDialog blocker(parent);
    blocker.setAttribute(Qt::WA_DontShowOnScreen);
    blocker.setWindowModality(Qt::ApplicationModal);
    // A shown dialog takes the focus from Live (see the end).
    const bool wasActive = QGuiApplication::applicationState() == Qt::ApplicationActive;
    bool lostFocus = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        lostFocus |= QGuiApplication::applicationState() != Qt::ApplicationActive;
        while (g_main_context_iteration(context, FALSE)) {
        }
        if (r.called && r.answered)
            blocker.accept();
    });
    poll.start(10);
    blocker.exec();
    poll.stop();

    g_dbus_connection_signal_unsubscribe(r.bus, r.subscription);
    while (g_main_context_iteration(context, FALSE)) {
    }
    g_main_context_unref(context);
    g_object_unref(r.bus);

    // Response 2 ("ended some other way") is both a backend that failed (GNOME's
    // without Nautilus: no dialog at all) and a dialog closed with Escape or its
    // window's close button (GTK's). Only a dialog that was shown took the focus.
    if (r.response == 1 || (r.response == 2 && r.called && wasActive && lostFocus)) {
        answer.status = PortalAnswer::Cancelled;
    } else if (r.response == 0 && r.results) {
        if (GVariant *uris = g_variant_lookup_value(r.results, "uris", G_VARIANT_TYPE_STRING_ARRAY)) {
            GVariantIter it;
            const gchar *uri;
            g_variant_iter_init(&it, uris);
            while (g_variant_iter_next(&it, "&s", &uri)) {
                const QString path = QUrl(QString::fromUtf8(uri)).toLocalFile();
                if (!path.isEmpty())
                    answer.paths << path;
            }
            g_variant_unref(uris);
        }
        if (GVariant *filter = g_variant_lookup_value(r.results, "current_filter", G_VARIANT_TYPE("(sa(us))"))) {
            const gchar *name = nullptr;
            g_variant_get(filter, "(&s@a(us))", &name, nullptr);
            answer.filter = QString::fromUtf8(name);
            g_variant_unref(filter);
        }
        // An answer without a local file (a remote location): as cancelled.
        answer.status = answer.paths.isEmpty() ? PortalAnswer::Cancelled : PortalAnswer::Ok;
    }
    if (r.results)
        g_variant_unref(r.results);
    menufixTrace("filedialogs: portal %c -> response %d, %d files\n", method[0], int(r.response), int(answer.paths.size()));
    menufixTrace("filedialogs: portal focus: active at start %d, lost %d\n", wasActive, lostFocus);
    if (answer.status == PortalAnswer::Failed) {
        portalFailed = true;
        menufixTrace("filedialogs: the portal's file chooser failed: Qt's own dialog from now on\n");
    }
    return answer;
}

GVariant *folderBytes(const QString &folder)
{
    return g_variant_new_bytestring(QFile::encodeName(folder).constData());
}

QString startDir(const std::string *dir)
{
    const QString d = fromPath(dir);
    return d.isEmpty() ? QDir::homePath() : d;
}

void throwSilent()
{
    void *exception = __cxa_allocate_exception(0x30);
    reinterpret_cast<void (*)(void *)>(base + kSilentException)(exception);
    __cxa_throw(exception, reinterpret_cast<void *>(base + kSilentExceptionType),
                reinterpret_cast<void (*)(void *)>(base + kSilentExceptionDtor));
}

void openFile(const void *title, const std::string *dir, int kind, void *receiver)
{
    const auto mask = reinterpret_cast<TypeMask (*)(int)>(base + kKindMask)(kind);
    const QStringList filters = filtersFor(mask);
    const QString file = [&] {
        LiveTicks ticks;
        if (usePortal()) {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
            addFilters(&options, filters, filters.value(0));
            g_variant_builder_add(&options, "{sv}", "current_folder", folderBytes(startDir(dir)));
            const PortalAnswer a = portalRequest("OpenFile", fromTText(title), &options);
            if (a.status != PortalAnswer::Failed)
                return a.paths.value(0);
        }
        return QFileDialog::getOpenFileName(dialogParent(), fromTText(title), startDir(dir),
                                            filters.join(QStringLiteral(";;")), nullptr, dialogOptions());
    }();
    menufixTrace("filedialogs: open kind %d -> %d\n", kind, !file.isEmpty());
    if (file.isEmpty())
        throwSilent();
    const std::string path = file.toStdString();
    virtualAt<SetPathFn>(receiver, 0x98)(receiver, &path);
}

int saveFile(const void *title, const void *name, const std::string *dir, uint64_t maskLo, uint64_t maskHi,
             void *receiver)
{
    const TypeMask mask{maskLo, maskHi};
    QStringList patterns;
    const QStringList filters = filtersFor(mask, &patterns);
    QString selected = filters.size() > 1 ? filters.at(1) : filters.value(0);
    const QString initial = QDir(startDir(dir)).filePath(fromTText(name));
    QString file = [&] {
        LiveTicks ticks;
        if (usePortal()) {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
            addFilters(&options, filters, selected);
            g_variant_builder_add(&options, "{sv}", "current_folder", folderBytes(startDir(dir)));
            g_variant_builder_add(&options, "{sv}", "current_name",
                                  g_variant_new_string(fromTText(name).toUtf8().constData()));
            const PortalAnswer a = portalRequest("SaveFile", fromTText(title), &options);
            if (a.status != PortalAnswer::Failed) {
                for (const QString &f : filters)
                    if (!a.filter.isEmpty() && f.startsWith(a.filter + QLatin1String(" (")))
                        selected = f;
                return a.paths.value(0);
            }
        }
        return QFileDialog::getSaveFileName(dialogParent(), fromTText(title), initial,
                                            filters.join(QStringLiteral(";;")), &selected, dialogOptions());
    }();
    menufixTrace("filedialogs: save -> %d\n", !file.isEmpty());
    if (file.isEmpty())
        throwSilent();
    // Add the extension of the chosen type when the name has none of the
    // allowed ones, as Live's own dialog does.
    bool known = patterns.isEmpty();
    for (const QString &p : patterns)
        known |= file.endsWith(p.mid(1), Qt::CaseInsensitive);
    if (!known) {
        const int open = selected.lastIndexOf('('), close = selected.lastIndexOf(')');
        const QString first = selected.mid(open + 1, close - open - 1).split(' ').value(0);
        if (first.startsWith(QLatin1String("*.")))
            file += first.mid(1);
    }
    const std::string path = file.toStdString();
    virtualAt<SetPathFn>(receiver, 0x98)(receiver, &path);
    const int type = reinterpret_cast<int (*)(const std::string *)>(base + kTypeOfPath)(&path);
    virtualAt<SetTypeFn>(receiver, 0xa0)(receiver, type, true);
    return virtualAt<ResultFn>(receiver, 0xa8)(receiver);
}

std::string *chooseFolder(std::string *result, const void *title, const std::string *dir, bool)
{
    const QString folder = [&] {
        LiveTicks ticks;
        if (usePortal()) {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(TRUE));
            g_variant_builder_add(&options, "{sv}", "current_folder", folderBytes(startDir(dir)));
            const PortalAnswer a = portalRequest("OpenFile", fromTText(title), &options);
            if (a.status != PortalAnswer::Failed)
                return a.paths.value(0);
        }
        return QFileDialog::getExistingDirectory(dialogParent(), fromTText(title), startDir(dir),
                                                 QFileDialog::ShowDirsOnly | dialogOptions());
    }();
    menufixTrace("filedialogs: folder -> %d\n", !folder.isEmpty());
    if (folder.isEmpty())
        throwSilent();
    return new (result) std::string(folder.toStdString());
}

uintptr_t mainExecutableBase()
{
    uintptr_t b = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr;
            return 1;
        },
        &b);
    return b;
}

} // namespace

// True once Live's own dialogs are in the table; then ours replace them.
bool registerDialogs()
{
    auto slot = [](uintptr_t at) { return *reinterpret_cast<uintptr_t *>(base + at); };
    if (slot(kOpenSlot) != base + kOpen || slot(kSaveSlot) != base + kSave || slot(kFolderSlot) != base + kFolder)
        return false;
    reinterpret_cast<void (*)(void *)>(base + kSetOpen)(reinterpret_cast<void *>(&openFile));
    reinterpret_cast<void (*)(void *)>(base + kSetSave)(reinterpret_cast<void *>(&saveFile));
    LiveFunction folder{reinterpret_cast<void *>(&chooseFolder), nullptr,
                        reinterpret_cast<void (*)(LiveFunction *, const LiveFunction *, int)>(base + kPointerManager),
                        reinterpret_cast<void *>(base + kPointerInvoker)};
    reinterpret_cast<void (*)(LiveFunction *)>(base + kSetFolder)(&folder);
    folder.manager(&folder, &folder, 3); // destroy the source, as Live does after its own call
    return true;
}

void installFileDialogs()
{
    if (!setAddresses())
        return;
    if (qEnvironmentVariable("LIVE_NATIVE_DIALOGS") == QLatin1String("0"))
        return;
    base = mainExecutableBase();
    // Live fills the table during its startup, after this runs: look from
    // the event loop until its dialogs are there (one minute at most).
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, [timer] {
        static int tries;
        const bool done = registerDialogs();
        if (done || ++tries >= 600) {
            menufixTrace("filedialogs: registered %d after %d tries\n", done, tries);
            timer->deleteLater();
        }
    });
    timer->start(100);
}
