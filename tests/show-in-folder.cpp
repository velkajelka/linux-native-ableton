// Checks for "Show in Folder" (shim/showinfolder.cpp), run by tests/show-in-folder.sh
// on a private session bus:
//   show-in-folder none         no file manager on the bus, then one that owns
//                               the name: detection false, then true; the
//                               ShowItems calls for five file names arrive as
//                               one URI each that decodes to the exact path,
//                               with an empty startup id
//   show-in-folder activatable  a service file (of another name) provides the
//                               file manager, nothing owns it: detection true
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QStringList>
#include <QUrl>
#include <cstdio>

bool fileManagerOnBus(const QDBusConnection &bus);
bool sendShowItems(const QDBusConnection &bus, const char *path);
void menufixTrace(const char *, int, int, int) {}

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

struct Call {
    QStringList uris;
    QString startup;
};

// Stands in for a file manager: records every ShowItems call it receives.
class StubFileManager : public QDBusVirtualObject {
public:
    QList<Call> calls;
    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name=\"org.freedesktop.FileManager1\"><method name=\"ShowItems\">"
                              "<arg type=\"as\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method></interface>");
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("org.freedesktop.FileManager1")
            || message.member() != QLatin1String("ShowItems"))
            return false;
        const QList<QVariant> args = message.arguments();
        calls.append({args.value(0).toStringList(), args.value(1).toString()});
        connection.send(message.createReply());
        return true;
    }
};

bool waitFor(const StubFileManager &stub, int count)
{
    QElapsedTimer t;
    t.start();
    while (stub.calls.size() < count && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return stub.calls.size() >= count;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QString();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        std::printf("FAIL: no session bus\n");
        return 1;
    }

    if (mode == QLatin1String("activatable")) {
        check(!bus.interface()->isServiceRegistered(QStringLiteral("org.freedesktop.FileManager1")),
              "the file manager is not running");
        check(fileManagerOnBus(bus), "a file manager the bus can start is found");
        check(!bus.interface()->isServiceRegistered(QStringLiteral("org.freedesktop.FileManager1")),
              "looking for it does not start it");
        return failures ? 1 : 0;
    }

    check(!fileManagerOnBus(bus), "no file manager on the bus: not found");

    // The file manager owns the name on its own connection.
    QDBusConnection owner = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("owner"));
    StubFileManager stub;
    if (!owner.registerVirtualObject(QStringLiteral("/org/freedesktop/FileManager1"), &stub)
        || !owner.registerService(QStringLiteral("org.freedesktop.FileManager1"))) {
        std::printf("FAIL: stub file manager did not start\n");
        return 1;
    }
    check(fileManagerOnBus(bus), "a running file manager is found");

    check(!sendShowItems(bus, nullptr), "no path: nothing sent");
    check(!sendShowItems(bus, ""), "empty path: nothing sent");

    const char *const paths[] = {
        "/m/Samples/Kick 01.wav",
        "/m/Samples/Caf\xc3\xa9 \xc3\x98.wav",
        "/m/Samples/Bass C#2.wav",
        "/m/Samples/Hat, open.wav",
        "/m/Samples/Pad 100%.wav",
    };
    int sent = 0;
    for (const char *p : paths)
        sent += sendShowItems(bus, p);
    check(sent == 5, "five paths sent");
    check(waitFor(stub, 5), "five ShowItems calls received");
    for (int i = 0; i < 5 && i < stub.calls.size(); ++i) {
        const Call &c = stub.calls.at(i);
        const QString expected = QString::fromUtf8(paths[i]);
        const QUrl url(c.uris.value(0), QUrl::StrictMode);
        const bool ok = c.uris.size() == 1 && url.isValid() && url.scheme() == QLatin1String("file")
                        && url.toLocalFile() == expected && c.startup.isEmpty();
        std::printf("  %s %s -> %s\n", ok ? "ok  " : "FAIL", paths[i], qPrintable(c.uris.join(QLatin1Char('|'))));
        if (!ok)
            ++failures;
    }
    return failures ? 1 : 0;
}
