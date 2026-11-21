// "Show in Folder" in Live's browser.
//
// The item stayed greyed out with file managers that register the
// org.freedesktop.FileManager1 bus name under a service file of another name
// (Dolphin: org.kde.dolphin.FileManager1.service), and a file name holding
// '#', ',' or '%' arrived cut short, split in two or empty.
//
// Live 12.4.7: the item is enabled when the session bus has
// org.freedesktop.FileManager1, running or startable (asked once per run,
// each query capped at 1 s). Choosing it sends ShowItems with the path as one
// fully encoded file:// URI and an empty startup id, without waiting for a
// reply; the bus starts the file manager if needed. Both parts go in or
// neither does (first bytes compared, as in hook.cpp). Other builds: nothing
// replaced; the trace says which.
#include "livebuild.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <cstdint>
#include <cstring>
#include <link.h>

bool redirectFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *target); // hook.cpp
void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
// Inside the shim only: not exported from libmenufix.so.
#define SHIM_LOCAL __attribute__((visibility("hidden")))
SHIM_LOCAL bool fileManagerOnBus(const QDBusConnection &bus);
SHIM_LOCAL bool sendShowItems(const QDBusConnection &bus, const char *path);
SHIM_LOCAL bool installShowInFolderAt(uintptr_t base);
SHIM_LOCAL void installShowInFolder();

namespace {

const QString kFileManager = QStringLiteral("org.freedesktop.FileManager1");

constexpr uintptr_t kAvailable = 0x1bf0c50;
constexpr uintptr_t kShow = 0x1b37850;
constexpr uintptr_t kMode = 0x1b27220;
constexpr int kModeAlwaysAvailable = 2;
const unsigned char kAvailablePrologue[] = {0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x58, 0xe8, 0xc4, 0x65, 0xf3, 0xff};
const unsigned char kShowPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                       0x53, 0x48, 0x81, 0xec, 0xb8, 0x02, 0x00, 0x00};

using ModeFn = int (*)();
ModeFn liveMode = nullptr;

// One org.freedesktop.DBus method returning a value, capped at 1 s; an
// invalid QVariant on any error or timeout.
QVariant askBus(const QDBusConnection &bus, const QString &method, const QVariantList &args)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                      QStringLiteral("org.freedesktop.DBus"), method);
    msg.setArguments(args);
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 1000);
    return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0) : QVariant();
}

bool available()
{
    static const bool onBus = fileManagerOnBus(QDBusConnection::sessionBus());
    return liveMode() == kModeAlwaysAvailable || onBus;
}

void show(const char *const *path)
{
    sendShowItems(QDBusConnection::sessionBus(), path ? *path : nullptr);
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

// True when the bus has a file manager: something owns
// org.freedesktop.FileManager1, or a service file lets the bus start one.
bool fileManagerOnBus(const QDBusConnection &bus)
{
    if (!bus.isConnected())
        return false;
    if (askBus(bus, QStringLiteral("NameHasOwner"), {kFileManager}).toBool())
        return true;
    return askBus(bus, QStringLiteral("ListActivatableNames"), {}).toStringList().contains(kFileManager);
}

// Asks the file manager to show `path` (UTF-8) selected in its folder. False
// when there is no path or the message could not be sent.
bool sendShowItems(const QDBusConnection &bus, const char *path)
{
    if (!path || !*path)
        return false;
    const QString uri = QUrl::fromLocalFile(QString::fromUtf8(path)).toString(QUrl::FullyEncoded);
    QDBusMessage msg = QDBusMessage::createMethodCall(kFileManager, QStringLiteral("/org/freedesktop/FileManager1"),
                                                      kFileManager, QStringLiteral("ShowItems"));
    msg << QStringList{uri} << QString();
    if (bus.send(msg))
        return true;
    menufixTrace("showinfolder: ShowItems not sent\n");
    return false;
}

// Replaces the two functions of a Live 12.4.7 image loaded at `base`: both,
// or neither when either one's first bytes differ.
bool installShowInFolderAt(uintptr_t base)
{
    liveMode = reinterpret_cast<ModeFn>(base + kMode);
    const bool bytesMatch = memcmp(reinterpret_cast<const void *>(base + kShow), kShowPrologue, sizeof kShowPrologue) == 0
                            && memcmp(reinterpret_cast<const void *>(base + kAvailable), kAvailablePrologue,
                                      sizeof kAvailablePrologue) == 0;
    const bool showDone = bytesMatch
                          && redirectFunction(base + kShow, kShowPrologue, sizeof kShowPrologue, reinterpret_cast<void *>(&show));
    const bool availableDone = showDone && redirectFunction(base + kAvailable, kAvailablePrologue, sizeof kAvailablePrologue,
                                                            reinterpret_cast<void *>(&available));
    menufixTrace("showinfolder: launch %d, check %d\n", showDone, availableDone);
    return availableDone;
}

void installShowInFolder()
{
    if (liveBuild() != LiveBuild::Live12_4_7) {
        menufixTrace("showinfolder: not Live 12.4.7, nothing replaced\n");
        return;
    }
    installShowInFolderAt(mainExecutableBase());
}
