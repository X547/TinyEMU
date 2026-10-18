/*
 * Audio through PipeWire
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "host_time.h"
#include "paced_audio.h"
#include "platform_backends.h"

/* The least the guest is kept ahead of the graph, whatever the latency asked
   for: enough to ride out a late tick of the event loop. */
#define PW_MIN_LEAD_MS 4
/* A graph that asks for nothing for this long no longer paces the guest; the
   host's clock does until it asks again. */
#define PW_STALL_MS 200
/* How long the ring can hold, beyond the lead. */
#define PW_RING_MS 250
/* How long the daemon may take to answer. */
#define PW_ROUNDTRIP_S 2


static std::string lower(std::string s)
{
    for (auto &c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}


struct PwSink {
    std::string name;
    std::string description;
};


/* One render stream. The guest's frames go into a ring from the event loop;
   the graph's data thread takes them out a quantum at a time, as the sink's
   clock asks for them, and what it has handed over is the clock the device
   runs by. */
class PipewireAudio final: public PacedAudio {
private:
    std::string fDeviceName;
    int fLatencyMs;
    bool fWarned = false;

    pw_thread_loop *fThreadLoop = nullptr;
    pw_context *fContext = nullptr;
    pw_core *fCore = nullptr;
    pw_registry *fRegistry = nullptr;
    pw_metadata *fSettings = nullptr;
    pw_stream *fStream = nullptr;
    spa_hook fCoreListener = {};
    spa_hook fRegistryListener = {};
    spa_hook fSettingsListener = {};
    spa_hook fStreamListener = {};

    /* the loop thread's, read under its lock */
    int fSyncSeq = 0;
    bool fSynced = false;
    bool fBroken = false;
    std::map<uint32_t, PwSink> fSinks;
    uint32_t fClockRate = 0;
    uint32_t fForceRate = 0;

    AudioRing fRing;

    /* The frames handed to the graph and when, for the device thread: a
       sequence lock, odd while the data thread updates them. */
    std::atomic<uint32_t> fClockSeq {0};
    std::atomic<uint64_t> fClockFrames {0};
    std::atomic<uint64_t> fClockUs {0};
    std::atomic<uint64_t> fMaxExtrapolate {0};

    /* the data thread's own */
    uint64_t fConsumed = 0;
    size_t fLead = 0;
    size_t fLeadLeft = 0;
    bool fLeadDone = false;
    unsigned fUnderruns = 0;

    static const pw_core_events kCoreEvents;
    static const pw_registry_events kRegistryEvents;
    static const pw_metadata_events kMetadataEvents;
    static const pw_stream_events kStreamEvents;

    static void CoreDone(void *data, uint32_t id, int seq);
    static void CoreError(void *data, uint32_t id, int seq, int res,
                          const char *message);
    static void RegistryGlobal(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const spa_dict *props);
    static void RegistryGlobalRemove(void *data, uint32_t id);
    static int SettingsProperty(void *data, uint32_t subject, const char *key,
                                const char *type, const char *value);
    static void StreamStateChanged(void *data, pw_stream_state old,
                                   pw_stream_state state, const char *error);
    static void StreamProcess(void *data);

    bool Roundtrip();
    std::string TargetName();
    uint64_t Extrapolate(uint64_t frames, uint64_t us, uint64_t now) const;
    void Process();
    void Fill(uint8_t *out, size_t frames);
    void Publish(uint64_t now);

protected:
    uint64_t ClockFrames() override;
    bool StartStream() override;
    void StopStream() override;

public:
    PipewireAudio(EventLoop &loop, const AudioSettings &settings);
    ~PipewireAudio() override;

    bool Init();

    AudioCaps Caps() override;
    void Write(const uint8_t *buf, size_t frames) override;
    void Read(uint8_t *buf, size_t frames) override
        {memset(buf, 0, frames * fFormat.FrameBytes());}
};


const pw_core_events PipewireAudio::kCoreEvents = [] {
    pw_core_events ev = {};
    ev.version = PW_VERSION_CORE_EVENTS;
    ev.done = CoreDone;
    ev.error = CoreError;
    return ev;
}();

const pw_registry_events PipewireAudio::kRegistryEvents = [] {
    pw_registry_events ev = {};
    ev.version = PW_VERSION_REGISTRY_EVENTS;
    ev.global = RegistryGlobal;
    ev.global_remove = RegistryGlobalRemove;
    return ev;
}();

const pw_metadata_events PipewireAudio::kMetadataEvents = [] {
    pw_metadata_events ev = {};
    ev.version = PW_VERSION_METADATA_EVENTS;
    ev.property = SettingsProperty;
    return ev;
}();

const pw_stream_events PipewireAudio::kStreamEvents = [] {
    pw_stream_events ev = {};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.state_changed = StreamStateChanged;
    ev.process = StreamProcess;
    return ev;
}();


PipewireAudio::PipewireAudio(EventLoop &loop, const AudioSettings &settings):
    PacedAudio(loop, settings.direction),
    fLatencyMs(settings.latency_ms)
{
    if (settings.device != nullptr) {
        fDeviceName = settings.device;
    }
}


PipewireAudio::~PipewireAudio()
{
    Stop();
    if (fThreadLoop != nullptr) {
        pw_thread_loop_stop(fThreadLoop);
    }
    if (fSettings != nullptr) {
        pw_proxy_destroy((pw_proxy *)fSettings);
    }
    if (fRegistry != nullptr) {
        pw_proxy_destroy((pw_proxy *)fRegistry);
    }
    if (fCore != nullptr) {
        pw_core_disconnect(fCore);
    }
    if (fContext != nullptr) {
        pw_context_destroy(fContext);
    }
    if (fThreadLoop != nullptr) {
        pw_thread_loop_destroy(fThreadLoop);
    }
}


bool PipewireAudio::Init()
{
    static std::once_flag once;
    std::call_once(once, []() {pw_init(nullptr, nullptr);});

    fThreadLoop = pw_thread_loop_new("audio", nullptr);
    if (fThreadLoop == nullptr) {
        return false;
    }
    /* Before 1.2 only client-rt.conf raises the data thread to real time
       priority; later ones do that from client.conf and may not ship the
       other. */
    pw_loop *loop = pw_thread_loop_get_loop(fThreadLoop);
    fContext = pw_context_new(loop,
                              pw_properties_new(PW_KEY_CONFIG_NAME,
                                                "client-rt.conf", nullptr),
                              0);
    if (fContext == nullptr) {
        fContext = pw_context_new(loop, nullptr, 0);
    }
    if (fContext == nullptr || pw_thread_loop_start(fThreadLoop) != 0) {
        return false;
    }

    pw_thread_loop_lock(fThreadLoop);
    bool ok = false;
    fCore = pw_context_connect(fContext, nullptr, 0);
    if (fCore == nullptr) {
        fprintf(stderr, "audio: no PipeWire daemon: %s\n", strerror(errno));
    } else {
        pw_core_add_listener(fCore, &fCoreListener, &kCoreEvents, this);
        fRegistry = pw_core_get_registry(fCore, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(fRegistry, &fRegistryListener,
                                 &kRegistryEvents, this);
        /* once for the globals, once more for what the settings hold */
        ok = Roundtrip() && Roundtrip();
        if (!ok) {
            fprintf(stderr, "audio: the PipeWire daemon does not answer\n");
        }
    }
    pw_thread_loop_unlock(fThreadLoop);
    return ok;
}


/* With the loop locked. */
bool PipewireAudio::Roundtrip()
{
    fSynced = false;
    fSyncSeq = pw_core_sync(fCore, PW_ID_CORE, fSyncSeq);
    while (!fSynced && !fBroken) {
        if (pw_thread_loop_timed_wait(fThreadLoop, PW_ROUNDTRIP_S) != 0) {
            return false;
        }
    }
    return fSynced;
}


AudioCaps PipewireAudio::Caps()
{
    AudioCaps caps;

    /* The graph's own rate: a stream at any other is resampled. */
    pw_thread_loop_lock(fThreadLoop);
    uint32_t rate = fForceRate != 0 ? fForceRate : fClockRate;
    pw_thread_loop_unlock(fThreadLoop);
    if (rate != 0) {
        caps.rate = rate;
    }
    return caps;
}


/* The sink whose description or name contains the one asked for; empty for
   the default one, which the session manager then keeps the stream on. With
   the loop locked. */
std::string PipewireAudio::TargetName()
{
    if (fDeviceName.empty()) {
        return std::string();
    }
    std::string want = lower(fDeviceName);
    for (const auto &it : fSinks) {
        if (lower(it.second.description).find(want) != std::string::npos ||
            lower(it.second.name).find(want) != std::string::npos) {
            return it.second.name;
        }
    }
    if (!fWarned) {
        fprintf(stderr, "audio: no output device matches '%s', using the "
                "default one\n", fDeviceName.c_str());
        fWarned = true;
    }
    return std::string();
}


//#pragma mark - loop thread

void PipewireAudio::CoreDone(void *data, uint32_t id, int seq)
{
    auto self = (PipewireAudio *)data;

    if (id == PW_ID_CORE && seq == self->fSyncSeq) {
        self->fSynced = true;
        pw_thread_loop_signal(self->fThreadLoop, false);
    }
}


void PipewireAudio::CoreError(void *data, uint32_t id, int seq, int res,
                              const char *message)
{
    auto self = (PipewireAudio *)data;

    (void)seq;
    if (id == PW_ID_CORE) {
        if (!self->fBroken) {
            fprintf(stderr, "audio: PipeWire: %s\n",
                    message != nullptr ? message : strerror(-res));
        }
        self->fBroken = true;
        pw_thread_loop_signal(self->fThreadLoop, false);
    }
}


void PipewireAudio::RegistryGlobal(void *data, uint32_t id,
                                   uint32_t permissions, const char *type,
                                   uint32_t version, const spa_dict *props)
{
    auto self = (PipewireAudio *)data;

    (void)permissions;
    (void)version;
    if (props == nullptr) {
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (cls != nullptr && name != nullptr &&
            strcmp(cls, "Audio/Sink") == 0) {
            self->fSinks[id] = {name, desc != nullptr ? desc : ""};
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
               self->fSettings == nullptr) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name != nullptr && strcmp(name, "settings") == 0) {
            self->fSettings = (pw_metadata *)pw_registry_bind(
                self->fRegistry, id, PW_TYPE_INTERFACE_Metadata,
                PW_VERSION_METADATA, 0);
            if (self->fSettings != nullptr) {
                pw_metadata_add_listener(self->fSettings,
                                         &self->fSettingsListener,
                                         &kMetadataEvents, self);
            }
        }
    }
}


void PipewireAudio::RegistryGlobalRemove(void *data, uint32_t id)
{
    auto self = (PipewireAudio *)data;

    self->fSinks.erase(id);
}


int PipewireAudio::SettingsProperty(void *data, uint32_t subject,
                                    const char *key, const char *type,
                                    const char *value)
{
    auto self = (PipewireAudio *)data;
    uint32_t v = value != nullptr ? (uint32_t)strtoul(value, nullptr, 10) : 0;

    (void)type;
    if (subject != PW_ID_CORE) {
        return 0;
    }
    /* no key: everything was cleared */
    if (key == nullptr || strcmp(key, "clock.rate") == 0) {
        self->fClockRate = v;
    }
    if (key == nullptr || strcmp(key, "clock.force-rate") == 0) {
        self->fForceRate = v;
    }
    return 0;
}


void PipewireAudio::StreamStateChanged(void *data, pw_stream_state old,
                                       pw_stream_state state,
                                       const char *error)
{
    (void)data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "audio: PipeWire stream: %s\n",
                error != nullptr ? error : "error");
    }
}


//#pragma mark - device side

/* Between two quanta the sink plays on at its rate, for two quanta at most.
   Past a stall the host's clock takes over from where that left off. */
uint64_t PipewireAudio::Extrapolate(uint64_t frames, uint64_t us,
                                    uint64_t now) const
{
    uint64_t max_ahead = fMaxExtrapolate.load(std::memory_order_relaxed);
    uint64_t elapsed = now > us ? now - us : 0;

    if (elapsed < (uint64_t)PW_STALL_MS * 1000) {
        uint64_t ahead = elapsed * fFormat.rate / 1000000;
        return frames + (ahead < max_ahead ? ahead : max_ahead);
    }
    return frames + max_ahead +
           (elapsed - (uint64_t)PW_STALL_MS * 1000) * fFormat.rate / 1000000;
}


uint64_t PipewireAudio::ClockFrames()
{
    uint64_t frames, us;
    uint32_t seq;

    do {
        seq = fClockSeq.load(std::memory_order_acquire);
        frames = fClockFrames.load(std::memory_order_relaxed);
        us = fClockUs.load(std::memory_order_relaxed);
    } while ((seq & 1) != 0 ||
             seq != fClockSeq.load(std::memory_order_acquire));

    return Extrapolate(frames, us, host_monotonic_us());
}


bool PipewireAudio::StartStream()
{
    uint32_t rate = fFormat.rate;
    size_t lead = (size_t)rate * PW_MIN_LEAD_MS / 1000;

    fRing.Reset(fFormat.FrameBytes(),
                lead + (size_t)rate * (fLatencyMs + PW_RING_MS) / 1000);
    fClockSeq.store(0);
    fClockFrames.store(0);
    fClockUs.store(fStartUs);
    fMaxExtrapolate.store((uint64_t)rate * 20 / 1000);
    fConsumed = 0;
    fLeadDone = false;
    fUnderruns = 0;

    /* Quanta of a quarter of the latency at most, so that the graph's own
       buffering leaves most of it to the guest's lead. */
    uint32_t quantum = 64;
    while (quantum * 2 <= rate * (uint32_t)fLatencyMs / 4000) {
        quantum *= 2;
    }

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_NAME, "TinyEMU",
        nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", quantum, rate);
    /* The rate the graph should run at, which is the one Caps() offered;
       anything else would be resampled. */
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", rate);

    spa_audio_info_raw info;
    memset(&info, 0, sizeof(info));
    switch (fFormat.ContainerBytes()) {
    case 1:
        info.format = SPA_AUDIO_FORMAT_S8;
        break;
    case 2:
        info.format = SPA_AUDIO_FORMAT_S16_LE;
        break;
    default:
        info.format = SPA_AUDIO_FORMAT_S32_LE;
        break;
    }
    info.rate = rate;
    info.channels = fFormat.channels;
    /* in the order of a WAVEFORMATEXTENSIBLE channel mask */
    static const uint32_t kPositions[] = {
        SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
        SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR,
        SPA_AUDIO_CHANNEL_FLC, SPA_AUDIO_CHANNEL_FRC,
    };
    if (fFormat.channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (fFormat.channels <= (int)SPA_N_ELEMENTS(kPositions)) {
        memcpy(info.position, kPositions,
               fFormat.channels * sizeof(kPositions[0]));
    } else {
        info.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
    }

    uint8_t pod_buf[1024];
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, pod_buf, sizeof(pod_buf));
    const spa_pod *params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info),
    };

    pw_thread_loop_lock(fThreadLoop);
    std::string target = TargetName();
    if (!target.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
    }
    int err = -EPIPE;
    fStream = fBroken ? nullptr : pw_stream_new(fCore, "TinyEMU", props);
    if (fStream != nullptr) {
        pw_stream_add_listener(fStream, &fStreamListener, &kStreamEvents,
                               this);
        err = pw_stream_connect(fStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                (pw_stream_flags)(
                                    PW_STREAM_FLAG_AUTOCONNECT |
                                    PW_STREAM_FLAG_MAP_BUFFERS |
                                    PW_STREAM_FLAG_RT_PROCESS),
                                params, 1);
        if (err < 0) {
            pw_stream_destroy(fStream);
            fStream = nullptr;
        }
    } else if (!fBroken) {
        err = -errno;
    } else {
        pw_properties_free(props);
    }
    pw_thread_loop_unlock(fThreadLoop);

    if (fStream == nullptr) {
        fprintf(stderr, "audio: could not open a PipeWire stream: %s\n",
                strerror(-err));
        return false;
    }
    return true;
}


void PipewireAudio::StopStream()
{
    /* Once it is destroyed, the data thread no longer reads the ring. */
    pw_thread_loop_lock(fThreadLoop);
    if (fStream != nullptr) {
        pw_stream_destroy(fStream);
        fStream = nullptr;
    }
    pw_thread_loop_unlock(fThreadLoop);
    if (fUnderruns != 0) {
        fprintf(stderr, "audio: the output ran dry %u times\n", fUnderruns);
    }
}


void PipewireAudio::Write(const uint8_t *buf, size_t frames)
{
    if (Running()) {
        fRing.Write(buf, frames);
    }
}


//#pragma mark - data thread

void PipewireAudio::StreamProcess(void *data)
{
    ((PipewireAudio *)data)->Process();
}


void PipewireAudio::Publish(uint64_t now)
{
    uint32_t seq = fClockSeq.load(std::memory_order_relaxed);
    fClockSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    fClockFrames.store(fConsumed, std::memory_order_relaxed);
    fClockUs.store(now, std::memory_order_relaxed);
    fClockSeq.store(seq + 2, std::memory_order_release);
}


void PipewireAudio::Process()
{
    pw_buffer *b = pw_stream_dequeue_buffer(fStream);
    if (b == nullptr) {
        return;
    }
    spa_data *d = &b->buffer->datas[0];
    size_t stride = fFormat.FrameBytes();
    size_t frames = d->maxsize / stride;
    if (b->requested != 0 && b->requested < frames) {
        frames = (size_t)b->requested;
    }
    if (d->data == nullptr || frames == 0) {
        pw_stream_queue_buffer(fStream, b);
        return;
    }

    /* After a stall the count goes on from what the host's clock told the
       guest meanwhile. */
    uint64_t now = host_monotonic_us();
    uint64_t last_us = fClockUs.load(std::memory_order_relaxed);
    if (now > last_us && now - last_us >= (uint64_t)PW_STALL_MS * 1000) {
        fConsumed = Extrapolate(fConsumed, last_us, now);
    }

    /* Two quanta are in the graph, the one played and the one filled; the
       rest of the latency asked for is the guest's lead. */
    if (!fLeadDone) {
        size_t lead = (size_t)fFormat.rate * fLatencyMs / 1000;
        size_t min_lead = (size_t)fFormat.rate * PW_MIN_LEAD_MS / 1000;
        lead = lead > 2 * frames ? lead - 2 * frames : 0;
        fLead = lead > min_lead ? lead : min_lead;
        fLeadLeft = fLead;
        fLeadDone = true;
    }
    fMaxExtrapolate.store(2 * frames, std::memory_order_relaxed);

    Fill((uint8_t *)d->data, frames);
    d->chunk->offset = 0;
    d->chunk->stride = (int32_t)stride;
    d->chunk->size = (uint32_t)(frames * stride);
    pw_stream_queue_buffer(fStream, b);

    fConsumed += frames;
    Publish(now);
}


/* The guest's lead first, then the ring. A ring that runs dry is made up for
   with silence. Whatever the ring holds beyond the lead and a little slack
   is dropped, so that the latency comes back to what it was however much of
   a stall the device made up for. */
void PipewireAudio::Fill(uint8_t *out, size_t frames)
{
    size_t frame_bytes = fFormat.FrameBytes();
    size_t done = 0;

    if (fLeadLeft > 0) {
        done = fLeadLeft < frames ? fLeadLeft : frames;
        memset(out, 0, done * frame_bytes);
        fLeadLeft -= done;
    } else {
        size_t keep = fLead + frames +
                      (size_t)fMaxExtrapolate.load(std::memory_order_relaxed);
        size_t avail = fRing.Available();
        if (avail > keep) {
            fRing.Read(nullptr, avail - keep);
        }
    }
    done += fRing.Read(out + done * frame_bytes, frames - done);
    if (done < frames) {
        memset(out + done * frame_bytes, 0, (frames - done) * frame_bytes);
        fUnderruns++;
    }
}


std::unique_ptr<HostAudio> host_audio_open(EventLoop &loop,
                                           const AudioSettings &settings)
{
    if (settings.direction != AUDIO_RENDER) {
        fprintf(stderr, "audio: capture from the host is not supported "
                "yet\n");
        return nullptr;
    }
    auto audio = std::make_unique<PipewireAudio>(loop, settings);
    if (!audio->Init()) {
        return nullptr;
    }
    return audio;
}
