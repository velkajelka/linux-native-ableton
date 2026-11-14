// PipeWire's graph timing, for the audio buffer decisions in alsaaudio.cpp.
//
// Live asks for a small ALSA ring: its fresh-install default is three periods
// of 128 frames, 2.9 ms in total. The pipewire-alsa plugin feeds the stream
// from that ring, and the graph cycles at its quantum: when the ring is
// shorter than a cycle, the device drains before the next cycle refills it,
// and every cycle ends in an XRUN, recovered and repeated (12,000 of them in
// the first minutes of a test install, one per cycle, no sound out).
//
// When the graph follows the streams, the cycle adapts to Live's ring instead
// and small settings work: the floor then only gets in the way, and the
// measure is skipped. When the quantum is fixed (clock.force-quantum), it
// stays whatever Live asks, and the ring has to cover it.
//
// The quantum is read from PipeWire's "settings" metadata, which carries the
// clock keys as plain strings: clock.force-quantum and clock.force-rate when
// the graph is pinned, clock.quantum and clock.rate otherwise (the graph's
// defaults; its cycle can still stretch while other clients run). One-shot
// connection with a deadline, dlopen'd like the Pulse client in
// pwdevices.cpp, kept for the process; a failed attempt is retried after
// five seconds. 0 = unknown: the caller applies no floor.
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <dlfcn.h>
#include <chrono>
#include <mutex>
#include <string>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

struct GraphTiming {
    unsigned quantum; // frames at the graph's rate; 0 = unknown, graph follows
    unsigned rate;
};

struct Api {
    void (*init)(int *argc, char **argv);
    struct pw_loop *(*loopNew)(const struct spa_dict *props);
    void (*loopDestroy)(struct pw_loop *loop);
    struct pw_context *(*contextNew)(struct pw_loop *loop, const struct spa_dict *props, size_t user_data_size);
    void (*contextDestroy)(struct pw_context *context);
    struct pw_core *(*connect)(struct pw_context *context, const struct spa_dict *props, size_t user_data_size);
    int (*coreDisconnect)(struct pw_core *core);
    struct pw_registry *(*getRegistry)(struct pw_core *core, uint32_t version, size_t user_data_size);
    // Bound globals take their events through the proxy, not through the
    // interface's own add_listener (that one is for server-side objects).
    void (*proxyAddObjectListener)(struct pw_proxy *proxy, struct spa_hook *listener, const void *events, void *data);
};

const Api *api()
{
    static Api api;
    static const bool loaded = [] {
        void *lib = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return false;
        bool ok = true;
        auto get = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        get(api.init, "pw_init");
        get(api.loopNew, "pw_loop_new");
        get(api.loopDestroy, "pw_loop_destroy");
        get(api.contextNew, "pw_context_new");
        get(api.contextDestroy, "pw_context_destroy");
        get(api.connect, "pw_context_connect");
        get(api.coreDisconnect, "pw_core_disconnect");
        get(api.getRegistry, "pw_core_get_registry");
        get(api.proxyAddObjectListener, "pw_proxy_add_object_listener");
        return ok;
    }();
    return loaded ? &api : nullptr;
}

// The clock keys, filled in by the metadata's property events as they arrive.
struct Settings {
    std::string forceQuantum, forceRate, quantum, rate;
    bool haveQuantum() const { return !forceQuantum.empty() || !quantum.empty(); }
    bool haveRate() const { return !forceRate.empty() || !rate.empty(); }
    bool done() const { return haveQuantum() && haveRate(); }
};

// The registry walk and the metadata binding it leads to; the listeners'
// hooks live here, for as long as the query iterates.
struct Session {
    Settings settings;
    struct pw_registry *registry = nullptr;
    bool bound = false;
    spa_hook metadataHook;
};

const pw_metadata_events metadataEvents = [] {
    pw_metadata_events events {};
    events.version = PW_VERSION_METADATA_EVENTS;
    events.property = [](void *data, uint32_t, const char *key, const char *, const char *value) {
        auto *settings = static_cast<Settings *>(data);
        if (!key || !value)
            return 0;
        const std::string text = value;
        if (!strcmp(key, "clock.force-quantum"))
            settings->forceQuantum = text;
        else if (!strcmp(key, "clock.force-rate"))
            settings->forceRate = text;
        else if (!strcmp(key, "clock.quantum"))
            settings->quantum = text;
        else if (!strcmp(key, "clock.rate"))
            settings->rate = text;
        return 0;
    };
    return events;
}();

const pw_registry_events registryEvents = [] {
    pw_registry_events events {};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = [](void *data, uint32_t id, uint32_t, const char *type, uint32_t, const struct spa_dict *props) {
        auto *session = static_cast<Session *>(data);
        if (session->bound || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
            return;
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || strcmp(name, "settings") != 0)
            return;
        session->bound = true;
        void *proxy = pw_registry_bind(session->registry, id, type, PW_VERSION_METADATA, 0);
        if (!proxy)
            return;
        if (const Api *pw = api())
            pw->proxyAddObjectListener(static_cast<struct pw_proxy *>(proxy), &session->metadataHook, &metadataEvents,
                                       &session->settings);
    };
    return events;
}();

bool queryTiming(GraphTiming &out)
{
    const Api *pw = api();
    if (!pw)
        return false;
    if (pw->init)
        pw->init(nullptr, nullptr);
    struct pw_loop *loop = pw->loopNew(nullptr);
    struct pw_context *context = loop ? pw->contextNew(loop, nullptr, 0) : nullptr;
    struct pw_core *core = context ? pw->connect(context, nullptr, 0) : nullptr;
    Session session;
    bool ok = false;
    if (core) {
        session.registry = pw->getRegistry(core, PW_VERSION_REGISTRY, 0);
        spa_hook registryHook;
        spa_zero(registryHook);
        pw_registry_add_listener(session.registry, &registryHook, &registryEvents, &session);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
        while (!session.settings.done()) {
            if (pw_loop_iterate(loop, 50) < 0)
                break;
            if (std::chrono::steady_clock::now() > deadline)
                break;
        }
        const std::string &q = session.settings.forceQuantum.empty() ? session.settings.quantum : session.settings.forceQuantum;
        const std::string &r = session.settings.forceRate.empty() ? session.settings.rate : session.settings.forceRate;
        if (!q.empty()) {
            out.quantum = strtoul(q.c_str(), nullptr, 10);
            out.rate = r.empty() ? 0 : strtoul(r.c_str(), nullptr, 10);
            ok = out.quantum != 0;
        }
        pw->coreDisconnect(core);
    }
    if (context)
        pw->contextDestroy(context);
    if (loop)
        pw->loopDestroy(loop);
    return ok;
}

} // namespace

// The graph's cycle: frames at the graph's rate, with the rate out when known.
// 0 = unknown: the graph follows the streams (or PipeWire did not answer) and
// the caller applies no floor.
unsigned pipewireGraphTiming(unsigned *rate)
{
    static GraphTiming graph {0, 0};
    static bool attempted = false;
    static std::chrono::steady_clock::time_point lastFailure;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (graph.quantum) {
        if (rate)
            *rate = graph.rate;
        return graph.quantum;
    }
    const auto now = std::chrono::steady_clock::now();
    if (attempted && now < lastFailure + std::chrono::seconds(5))
        return 0; // the last attempt just failed; try again later
    attempted = true;
    GraphTiming fresh {0, 0};
    if (queryTiming(fresh)) {
        graph = fresh;
        menufixTrace("pwgraph: quantum %u frames at %u Hz\n", graph.quantum, graph.rate);
    } else {
        lastFailure = now;
        menufixTrace("pwgraph: no quantum from PipeWire\n", 0, 0, 0);
    }
    if (graph.quantum && rate)
        *rate = graph.rate;
    return graph.quantum;
}
