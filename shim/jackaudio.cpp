// Driver type JACK (audiodrivers.cpp): Live's ALSA engine on alsa-plugins' JACK PCM.
//
// Live has no JACK driver (its AJackAudioDevice classes are unused stubs that
// call no libjack function), only an ALSA one. Opening the "JACK" device
// (card 200, alsaaudio.cpp) opens alsa-plugins' "jack" PCM type
// (libasound_module_pcm_jack.so: libasound2-plugins / alsa-plugins(-jack)),
// set up here instead of in a user's asoundrc:
//   name          "Ableton Live" (playback), "Ableton Live in" (capture)
//   *_ports       one port per channel (out_000.., in_000..), port i connected
//                 to the default device's port i (LIVE_JACK_CONNECT=0: none)
//   align_psize   false: any period, not only the JACK buffer size (which
//                 the plugin reads once, at open, before Live asks for one)
// The plugin carries the audio (its own process callback and ring buffer);
// alsaaudio.cpp gives it a buffer of 2 periods like the PipeWire devices.
//
// What the plugin does not do: the graph's buffer size. The plugin buffers
// whatever the cycle is, so a 128-frame Live period under PipeWire's idle
// 1024-frame quantum would underrun every cycle. A small client "Ableton Live
// buffer size" without ports asks the server for Live's period
// (jack_set_buffer_size) from snd_pcm_hw_params and lets go when Live closes
// its last JACK PCM (pipewire-jack: a node property, gone with the client;
// jackd: the server's setting, left as Live set it).
// LIVE_JACK_FORCE_QUANTUM=0 leaves the graph's buffer size alone.
// Under PipeWire all of Live's JACK clients share a node.group, so that
// client's quantum applies to the graph the plugin's clients run in.
//
// Input timing: the plugin runs capture and playback as two JACK clients, in no
// fixed order within a cycle. Live wakes on playback and reads the oldest
// capture period, so how the first cycles ran decides, for the whole run,
// whether Live gets the previous cycle's input (round trip 3 periods, what
// Live shows and compensates) or the one before (4 periods). The shim wraps
// the plugin's process callbacks to note the cycle each stream last ran in,
// and when Live reads capture it drops or repeats one period so the oldest
// unread period is the previous cycle's (alignCapture). Normally once, at
// start or after an xrun. LIVE_JACK_INPUT_ALIGN=0 leaves the input as is.
//
// Channels: as many as the default device has (pipewire-jack lists its
// ports first), LIVE_JACK_OUTPUTS / LIVE_JACK_INPUTS override; 2 until a
// JACK PCM was opened once (listing must not start a client: the JACK driver
// type is scanned at every start). The PCM gets the count Live's list showed.
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <jack/jack.h>
#include <dlfcn.h>
#include <unistd.h>
#include "pwdevices.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
void alsaPcmClosed(snd_pcm_t *pcm);                       // alsaaudio.cpp
void setLive11Channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, const void *returnAddress); // alsaaudio.cpp

namespace {

// libjack, loaded on first use so the shim also runs where JACK is missing.
struct JackApi {
    decltype(&jack_client_open) client_open;
    decltype(&jack_client_close) client_close;
    decltype(&jack_set_buffer_size) set_buffer_size;
    decltype(&jack_activate) activate;
    decltype(&jack_deactivate) deactivate;
    decltype(&jack_get_ports) get_ports;
    decltype(&jack_free) free;
    decltype(&jack_on_shutdown) on_shutdown;
    decltype(&jack_get_buffer_size) get_buffer_size;
    decltype(&jack_set_process_callback) set_process_callback;
    decltype(&jack_last_frame_time) last_frame_time;

    bool load()
    {
        void *lib = dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return false;
        bool ok = true;
        auto get = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        get(client_open, "jack_client_open");
        get(client_close, "jack_client_close");
        get(set_buffer_size, "jack_set_buffer_size");
        get(activate, "jack_activate");
        get(deactivate, "jack_deactivate");
        get(get_ports, "jack_get_ports");
        get(free, "jack_free");
        get(on_shutdown, "jack_on_shutdown");
        get(get_buffer_size, "jack_get_buffer_size");
        get(set_process_callback, "jack_set_process_callback");
        get(last_frame_time, "jack_last_frame_time");
        return ok;
    }
};

JackApi jack;

bool jackLoaded()
{
    static const bool loaded = jack.load();
    return loaded;
}
std::mutex jackMutex;                // everything below
jack_client_t *control;              // "Ableton Live buffer size"
bool controlActive;
jack_client_t *lostControl;          // the control client of a server that went away (not closed: libjack may still use it)
std::vector<snd_pcm_t *> pcms;       // Live's open JACK PCMs
std::vector<std::string> outPorts, inPorts; // the default device's ports
unsigned channelCount(const char *env, size_t ports);
unsigned outputs = channelCount("LIVE_JACK_OUTPUTS", 0), inputs = channelCount("LIVE_JACK_INPUTS", 0); // patch: env vars from start

// One of the plugin's clients: its process callback, and the cycle it last ran in.
struct Tap {
    JackProcessCallback process;
    void *arg;
    snd_pcm_ioplug_t *io;
    snd_pcm_t *pcm;
    jack_client_t *client;
    std::atomic<jack_nframes_t> cycle{0};  // frame time at the start of that cycle
    std::atomic<jack_nframes_t> frames{0}; // its length
    std::atomic<bool> ran{false};          // ran while the PCM was running
    std::atomic<unsigned> seq{0};          // odd while the plugin's callback runs
};
// The PCMs Live has open (one of each). Taps are not freed: Live's audio thread
// may still look at one while the PCM closes; they are a few bytes per open.
std::atomic<Tap *> playTap, capTap;

int tapProcess(jack_nframes_t frames, void *arg)
{
    // The cycle is noted before the plugin runs: its playback callback wakes Live,
    // which may read it before the callback returns. seq tells a reader that the
    // capture callback is midway (its frames may or may not be in yet).
    Tap *tap = static_cast<Tap *>(arg);
    tap->seq.fetch_add(1, std::memory_order_acq_rel);
    tap->frames.store(frames, std::memory_order_relaxed);
    tap->cycle.store(jack.last_frame_time(tap->client), std::memory_order_release);
    const int result = tap->process(frames, tap->arg);
    tap->ran.store(tap->io->state == SND_PCM_STATE_RUNNING, std::memory_order_release);
    tap->seq.fetch_add(1, std::memory_order_release);
    return result;
}

// Called from Live's capture snd_pcm_avail_update (Live's audio thread only), avail >= 0.

// Live 11's sample formats. Live 11 offers its ALSA device S32_LE, S24_3LE and
// S16_LE only (0x25dcaaa); the plugin's PCM takes FLOAT_LE only, so opening the
// JACK device failed (snd_pcm_hw_params_set_format_mask: Invalid argument).
// Live 12 offers FLOAT_LE as well. For Live 11 the PCM gets FLOAT_LE
// (jackSetFloatFormat), Live is told S32_LE (alsaaudio.cpp's
// snd_pcm_hw_params_get_format), and the samples are converted in place in the
// plugin's ring buffer, both 4 bytes: playback S32 -> float when Live commits
// frames (before the plugin can read them), capture float -> S32 when Live maps
// frames the plugin wrote (mmap_begin), each frame once. Capture counts frames
// from the last prepare: taken by Live (commit, and alignCapture's forward or
// rewind) and converted.
struct FloatPcm {
    snd_pcm_t *pcm;
    const snd_pcm_channel_area_t *areas;
    unsigned channels;
    uint64_t taken, converted; // capture
};
FloatPcm floatPcms[2]; // playback, capture

FloatPcm *floatPcm(snd_pcm_t *pcm)
{
    for (FloatPcm &f : floatPcms)
        if (f.pcm && f.pcm == pcm)
            return &f;
    return nullptr;
}

void convertFrames(const FloatPcm &f, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames, bool toFloat)
{
    for (unsigned c = 0; c < f.channels; ++c) {
        const snd_pcm_channel_area_t &area = f.areas[c];
        for (snd_pcm_uframes_t i = 0; i < frames; ++i) {
            char *at = static_cast<char *>(area.addr) + (area.first + (offset + i) * area.step) / 8;
            if (toFloat) {
                int32_t sample;
                memcpy(&sample, at, 4);
                const float value = float(sample) * (1.0f / 2147483648.0f);
                memcpy(at, &value, 4);
            } else {
                float value;
                memcpy(&value, at, 4);
                const int32_t sample = value >= 1.0f ? INT32_MAX : value <= -1.0f ? INT32_MIN : int32_t(value * 2147483648.0f);
                memcpy(at, &sample, 4);
            }
        }
    }
}

// Live runs once per cycle, woken by playback (cycle P), and reads one period from the
// oldest unread capture frames. Those should come from cycle P - period. The newest
// captured period is from cycle C (C = P when capture ran first this cycle, else
// P - period), so the oldest is C - avail + period, and Live's input is older than it
// should be by (P - C) + avail - 2 * period frames.
snd_pcm_sframes_t alignCapture(Tap *cap, snd_pcm_sframes_t avail, snd_pcm_sframes_t (*availUpdate)(snd_pcm_t *))
{
    static const bool off = getenv("LIVE_JACK_INPUT_ALIGN") && !strcmp(getenv("LIVE_JACK_INPUT_ALIGN"), "0");
    static long pending, streak; // the same misalignment seen in a row
    Tap *play = playTap.load(std::memory_order_acquire);
    const snd_pcm_sframes_t period = cap->io->period_size;
    if (off || !play || !play->ran.load(std::memory_order_acquire) || !cap->ran.load(std::memory_order_acquire)
        || play->io->state != SND_PCM_STATE_RUNNING || cap->io->state != SND_PCM_STATE_RUNNING
        || play->frames.load(std::memory_order_relaxed) != period || cap->frames.load(std::memory_order_relaxed) != period
        || availUpdate(play->pcm) != period) { // Live late, or the graph not at Live's period: leave it
        streak = 0;
        return avail;
    }
    // A consistent view of capture: not while its callback runs, nor if it ran meanwhile.
    const unsigned seq = cap->seq.load(std::memory_order_acquire);
    if (seq & 1)
        return avail;
    const jack_nframes_t capCycle = cap->cycle.load(std::memory_order_acquire);
    avail = availUpdate(cap->pcm);
    if (avail < 0 || cap->seq.load(std::memory_order_acquire) != seq)
        return avail;
    const int32_t sinceCapture = int32_t(play->cycle.load(std::memory_order_acquire) - capCycle);
    const snd_pcm_sframes_t late = sinceCapture + avail - 2 * period;
    if (late == 0 || (sinceCapture != 0 && sinceCapture != period)) { // aligned, or a missed cycle
        streak = 0;
        return avail;
    }
    streak = late == pending ? streak + 1 : 1;
    pending = late;
    if (streak < 3) // a few cycles in a row, not a passing state
        return avail;
    streak = 0;
    snd_pcm_sframes_t moved = 0;
    if (late > 0 && late <= avail - period)
        moved = snd_pcm_forward(cap->pcm, snd_pcm_uframes_t(late));   // drop the older input
    else if (late < 0 && -late <= snd_pcm_rewindable(cap->pcm))
        moved = -snd_pcm_rewind(cap->pcm, snd_pcm_uframes_t(-late)); // repeat a period
    if (FloatPcm *f = floatPcm(cap->pcm))
        f->taken += moved;
    menufixTrace("jackaudio: input %d frames late, moved %d\n", int(late), int(moved));
    return availUpdate(cap->pcm);
}

// Ports of the default device (pipewire-jack lists them first), up to the next client name.
std::vector<std::string> defaultDevicePorts(unsigned long flags)
{
    std::vector<std::string> ports;
    const char **names = jack.get_ports(control, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | flags);
    if (names && names[0]) {
        const std::string device(names[0], strcspn(names[0], ":"));
        for (unsigned n = 0; names[n] && ports.size() < 64; ++n)
            if (!strncmp(names[n], device.c_str(), device.size()) && names[n][device.size()] == ':')
                ports.push_back(names[n]);
    }
    if (names)
        jack.free(names);
    return ports;
}

unsigned channelCount(const char *env, size_t ports)
{
    if (const char *v = getenv(env))
        return std::clamp(atoi(v), 1, 64);
    return ports ? unsigned(ports) : 2;
}

// Opens the control client (once per server) and reads the default device's ports.
bool openControl()
{
    if (control && control != lostControl)
        return true;
    control = nullptr;
    controlActive = false;
    if (!jackLoaded()) {
        menufixTrace("jackaudio: no libjack.so.0\n");
        return false;
    }
    jack_status_t status;
    control = jack.client_open("Ableton Live buffer size", JackNoStartServer, &status);
    if (!control) {
        menufixTrace("jackaudio: no JACK server (status 0x%x)\n", status);
        return false;
    }
    jack.on_shutdown(control, [](void *client) {
        lostControl = static_cast<jack_client_t *>(client); // a pointer store; the next open makes a new client
        menufixTrace("jackaudio: the JACK server went away\n");
    }, control);
    outPorts = defaultDevicePorts(JackPortIsInput);
    inPorts = defaultDevicePorts(JackPortIsOutput);
    outputs = channelCount("LIVE_JACK_OUTPUTS", outPorts.size());
    inputs = channelCount("LIVE_JACK_INPUTS", inPorts.size());
    menufixTrace("jackaudio: %d outputs, %d inputs\n", outputs, inputs);
    return true;
}

std::string quoted(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

// The plugin's configuration, as an asoundrc would give it.
std::string pcmConfig(bool playback, unsigned channels)
{
    const bool connect = !(getenv("LIVE_JACK_CONNECT") && !strcmp(getenv("LIVE_JACK_CONNECT"), "0"));
    const std::vector<std::string> &device = playback ? outPorts : inPorts;
    std::string conf = "pcm.live { type jack align_psize false name ";
    conf += quoted(playback ? "Ableton Live" : "Ableton Live in");
    conf += playback ? " playback_ports { " : " capture_ports { ";
    for (unsigned c = 0; c < channels; ++c)
        conf += std::to_string(c) + (connect && c < device.size() ? " " + quoted(device[c]) : " { }") + " ";
    return conf + "} }";
}

template <typename Fn>
Fn real(const char *name, const char *version = "ALSA_0.9")
{
    return reinterpret_cast<Fn>(nextSymbol(name, version));
}

#define REAL(name, version) static const auto realFn = real<decltype(&name)>(#name, version)

bool isJack(snd_pcm_t *pcm)
{
    return std::find(pcms.begin(), pcms.end(), pcm) != pcms.end();
}

} // namespace

bool jackStream(snd_pcm_t *pcm)
{
    std::lock_guard<std::mutex> lock(jackMutex);
    return isJack(pcm);
}

unsigned jackChannels(bool playback)
{
    std::lock_guard<std::mutex> lock(jackMutex);
    return playback ? outputs : inputs;
}

// Called by alsaaudio.cpp's snd_pcm_open for the JACK device.
int jackOpen(snd_pcm_t **pcm, snd_pcm_stream_t stream)
{
    std::lock_guard<std::mutex> lock(jackMutex);
    const unsigned listed = stream == SND_PCM_STREAM_PLAYBACK ? outputs : inputs; // before openControl updates it
    // All of Live's JACK clients in one PipeWire group (pipewire-jack reads PIPEWIRE_PROPS at client open).
    const char *props = getenv("PIPEWIRE_PROPS");
    const std::string saved = props ? props : "";
    setenv("PIPEWIRE_PROPS", "{ node.group = ableton-live }", 1);
    auto restore = [&] { props ? setenv("PIPEWIRE_PROPS", saved.c_str(), 1) : unsetenv("PIPEWIRE_PROPS"); };
    if (!openControl()) {
        restore();
        return -ENODEV;
    }
    const std::string text = pcmConfig(stream == SND_PCM_STREAM_PLAYBACK, listed);
    snd_config_t *top = nullptr;
    snd_input_t *in = nullptr;
    int err = snd_config_top(&top);
    if (err >= 0)
        err = snd_input_buffer_open(&in, text.data(), text.size());
    if (err >= 0)
        err = snd_config_load(top, in);
    if (in)
        snd_input_close(in);
    if (err >= 0)
        err = snd_pcm_open_lconf(pcm, "live", stream, 0, top);
    restore();
    if (top)
        snd_config_delete(top); // the plugin copied what it needs
    if (err < 0) {
        menufixTrace("jackaudio: opening the JACK PCM failed (%d)\n", err);
        return err == -ENOENT ? -ENODEV : err;
    }
    pcms.push_back(*pcm);
    menufixTrace(stream == SND_PCM_STREAM_PLAYBACK ? "jackaudio: playback open, %d channels\n" : "jackaudio: capture open, %d channels\n", listed);
    return 0;
}

// The plugin's channel count is its number of ports.
int jackSetChannels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    using SetChannelsFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
    static auto realFn = real<SetChannelsFn>("snd_pcm_hw_params_set_channels");
    unsigned ports = 0;
    snd_pcm_hw_params_get_channels_max(params, &ports);
    if (ports && val != ports)
        menufixTrace("jackaudio: %d channels instead of %d\n", ports, val);
    return realFn(pcm, params, ports ? ports : val);
}

// Live 11: FLOAT_LE on the JACK PCM (see FloatPcm). Called by alsaaudio.cpp's
// snd_pcm_hw_params_set_format_mask instead of Live's S32/S24/S16 mask.
int jackSetFloatFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    const int result = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_FLOAT_LE);
    if (result == 0) {
        FloatPcm &f = floatPcms[snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE];
        f = {pcm, nullptr, 0, 0, 0};
        menufixTrace("jackaudio: float samples, converted for Live 11\n");
    }
    return result;
}

extern "C" int snd_pcm_mmap_begin(snd_pcm_t *pcm, const snd_pcm_channel_area_t **areas, snd_pcm_uframes_t *offset,
                                  snd_pcm_uframes_t *frames)
{
    REAL(snd_pcm_mmap_begin, "ALSA_0.9");
    const int result = realFn(pcm, areas, offset, frames);
    FloatPcm *f = result >= 0 ? floatPcm(pcm) : nullptr;
    if (!f)
        return result;
    f->areas = *areas;
    if (f == &floatPcms[1]) {
        const uint64_t end = f->taken + *frames;
        if (f->converted < end) {
            const uint64_t from = f->converted > f->taken ? f->converted : f->taken;
            convertFrames(*f, *offset + (from - f->taken), end - from, false);
            f->converted = end;
        }
    }
    return result;
}

extern "C" snd_pcm_sframes_t snd_pcm_mmap_commit(snd_pcm_t *pcm, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
{
    REAL(snd_pcm_mmap_commit, "ALSA_0.9");
    FloatPcm *f = floatPcm(pcm);
    if (f && f->areas && f == &floatPcms[0])
        convertFrames(*f, offset, frames, true);
    const snd_pcm_sframes_t result = realFn(pcm, offset, frames);
    if (f && f == &floatPcms[1] && result > 0)
        f->taken += snd_pcm_uframes_t(result);
    return result;
}

extern "C" int snd_pcm_prepare(snd_pcm_t *pcm)
{
    REAL(snd_pcm_prepare, "ALSA_0.9");
    if (FloatPcm *f = floatPcm(pcm))
        f->taken = f->converted = 0;
    return realFn(pcm);
}

// The plugin's clients (libasound_module_pcm_jack.so registers its callback from
// snd_pcm_prepare): wrapped to note their cycles (Tap). Any other caller passes through.
extern "C" int jack_set_process_callback(jack_client_t *client, JackProcessCallback process, void *arg)
{
    if (!jackLoaded())
        return -1;
    Dl_info info;
    if (process && arg && dladdr(reinterpret_cast<void *>(process), &info) && info.dli_fname
        && strstr(info.dli_fname, "libasound_module_pcm_jack")) {
        auto *io = static_cast<snd_pcm_ioplug_t *>(arg);
        Tap *tap = new Tap;
        tap->process = process;
        tap->arg = arg;
        tap->io = io;
        tap->pcm = io->pcm;
        tap->client = client;
        const int result = jack.set_process_callback(client, tapProcess, tap);
        if (result == 0)
            (io->stream == SND_PCM_STREAM_PLAYBACK ? playTap : capTap).store(tap, std::memory_order_release);
        return result;
    }
    return jack.set_process_callback(client, process, arg);
}

// Versions as Live references them (shim/qt5.map).
extern "C" snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
    REAL(snd_pcm_avail_update, "ALSA_0.9");
    const snd_pcm_sframes_t avail = realFn(pcm);
    Tap *cap = capTap.load(std::memory_order_acquire);
    return avail >= 0 && cap && cap->pcm == pcm ? alignCapture(cap, avail, realFn) : avail;
}

extern "C" int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    REAL(snd_pcm_hw_params, "ALSA_0.9");
    setLive11Channels(pcm, params, __builtin_return_address(0));
    const int result = realFn(pcm, params);
    if (FloatPcm *f = result == 0 ? floatPcm(pcm) : nullptr) {
        unsigned channels = 0;
        snd_pcm_hw_params_get_channels(params, &channels);
        f->channels = channels;
        f->taken = f->converted = 0;
    }
    std::lock_guard<std::mutex> lock(jackMutex);
    if (result < 0 || !isJack(pcm) || !control || control == lostControl || (getenv("LIVE_JACK_FORCE_QUANTUM") && !strcmp(getenv("LIVE_JACK_FORCE_QUANTUM"), "0")))
        return result;
    snd_pcm_uframes_t period = 0;
    if (snd_pcm_hw_params_get_period_size(params, &period, nullptr) >= 0 && period) {
        if (!controlActive) // pipewire-jack applies a client's buffer size while it is active
            controlActive = jack.activate(control) == 0;
        if (jack.set_buffer_size(control, jack_nframes_t(period)) != 0)
            menufixTrace("jackaudio: the server refused buffer size %d\n", int(period));
        // Wait (up to 0.5 s) until the graph runs at it: Live starting while the old, longer
        // cycles still run drains the plugin's buffer and costs a period of latency.
        int waited = 0;
        while (jack.get_buffer_size(control) != period && waited < 500) {
            usleep(10000);
            waited += 10;
        }
        menufixTrace("jackaudio: graph buffer size %d (now %d after %d ms)\n", int(period), int(jack.get_buffer_size(control)), waited);
    }
    return result;
}

extern "C" int snd_pcm_close(snd_pcm_t *pcm)
{
    REAL(snd_pcm_close, "ALSA_0.9");
    {
        std::lock_guard<std::mutex> lock(jackMutex);
        auto it = std::find(pcms.begin(), pcms.end(), pcm);
        if (it != pcms.end()) {
            if (FloatPcm *f = floatPcm(pcm))
                *f = {};
            for (std::atomic<Tap *> *tap : {&playTap, &capTap})
                if (Tap *t = tap->load(); t && t->pcm == pcm)
                    tap->store(nullptr, std::memory_order_release);
            pcms.erase(it);
            if (pcms.empty() && controlActive && control != lostControl) {
                jack.deactivate(control);
                controlActive = false;
            }
            return realFn(pcm);
        }
    }
    alsaPcmClosed(pcm); // alsaaudio.cpp: hands a reserved sound card back
    return realFn(pcm);
}
