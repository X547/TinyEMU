/*
 * Haiku audio through the Media Kit
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
#include <Buffer.h>
#include <BufferGroup.h>
#include <BufferProducer.h>
#include <MediaEventLooper.h>
#include <MediaRoster.h>
#include <TimeSource.h>

#include <stdio.h>
#include <string.h>

#include <atomic>

#include "paced_audio.h"
#include "platform_backends.h"

#define SEND_BUFFER_EVENT (BTimedEventQueue::B_USER_EVENT + 1)

/* How far beyond the chain's latency the guest is kept ahead, so that a late
   tick of the event loop is not heard. */
#define MEDIA_LEAD_MARGIN_US 10000
/* What the ring holds beyond the lead. */
#define MEDIA_RING_MS 500
/* The largest buffer sent, whatever the latency asked for. */
#define MEDIA_MAX_BUFFER_US 10000


class MediaAudio;

/* A producer with one output, which sends the guest's frames downstream a
   buffer at a time, each stamped with the performance time it is to be
   played at. */
class AudioProducerNode final: public BBufferProducer,
                               public BMediaEventLooper {
private:
    MediaAudio *fOwner;
    media_output fOutput;
    media_multi_audio_format fWanted;
    BBufferGroup *fBufferGroup = nullptr;
    bool fOutputEnabled = true;
    bigtime_t fLatency = 0;          /* downstream */
    bigtime_t fInternalLatency = 0;
    bigtime_t fStartTime = 0;
    uint64 fFramesSent = 0;

    size_t BufferFrames() const;
    void UpdateLatency();
    status_t AllocateBuffers();
    void SendNewBuffer(const media_timed_event *event);

public:
    AudioProducerNode(const char *name, MediaAudio *owner);
    ~AudioProducerNode() override;

    /* The format the next connection must have. */
    void SetWanted(const media_multi_audio_format &format) {fWanted = format;}

    /* BMediaNode */
    BMediaAddOn *AddOn(int32 *internal_id) const override;
    status_t HandleMessage(int32 message, const void *data,
                           size_t size) override;

protected:
    void NodeRegistered() override;

    /* BBufferProducer */
    status_t FormatSuggestionRequested(media_type type, int32 quality,
                                       media_format *format) override;
    status_t FormatProposal(const media_source &output,
                            media_format *format) override;
    status_t FormatChangeRequested(const media_source &source,
                                   const media_destination &destination,
                                   media_format *format,
                                   int32 *deprecated) override;
    status_t GetNextOutput(int32 *cookie, media_output *output) override;
    status_t DisposeOutputCookie(int32 cookie) override;
    status_t SetBufferGroup(const media_source &source,
                            BBufferGroup *group) override;
    status_t GetLatency(bigtime_t *latency) override;
    status_t PrepareToConnect(const media_source &what,
                              const media_destination &where,
                              media_format *format, media_source *source,
                              char *name) override;
    void Connect(status_t error, const media_source &source,
                 const media_destination &destination,
                 const media_format &format, char *name) override;
    void Disconnect(const media_source &what,
                    const media_destination &where) override;
    void LateNoticeReceived(const media_source &what, bigtime_t how_much,
                            bigtime_t performance_time) override;
    void EnableOutput(const media_source &what, bool enabled,
                      int32 *deprecated) override;
    void LatencyChanged(const media_source &source,
                        const media_destination &destination,
                        bigtime_t new_latency, uint32 flags) override;

    /* BMediaEventLooper */
    void HandleEvent(const media_timed_event *event, bigtime_t lateness,
                     bool real_time_event = false) override;
};


/* One render stream into the system mixer. The node's time source is the
   one the mixer and the sound card run on, and the device's clock is read
   from it, so the guest moves at the rate the card plays. */
class MediaAudio final: public PacedAudio {
private:
    int fLatencyMs;
    AudioCaps fCaps;
    AudioProducerNode *fNode = nullptr;
    media_node fMixer;
    bool fHaveMixer = false;
    bool fConnected = false;
    media_output fOutput;
    media_input fInput;
    media_multi_audio_format fConnectedFormat;
    AudioRing fRing;

    /* Where the stream's first buffer plays, in performance time; 0 while
       it is stopped. */
    std::atomic<bigtime_t> fStartTime {0};
    /* How far ahead of what plays the guest has to be. */
    std::atomic<bigtime_t> fLeadUs {0};

    /* the node thread's own */
    size_t fDebt = 0;
    unsigned fUnderruns = 0;

    media_multi_audio_format MediaFormat() const;
    bool ConnectNode(const media_multi_audio_format &format);
    void DisconnectNode();

protected:
    uint64_t ClockFrames() override;
    bool StartStream() override;
    void StopStream() override;

public:
    MediaAudio(EventLoop &loop, const AudioSettings &settings);
    ~MediaAudio() override;

    bool Init();

    AudioCaps Caps() override {return fCaps;}
    void Write(const uint8_t *buf, size_t frames) override;
    void Read(uint8_t *buf, size_t frames) override
        {memset(buf, 0, frames * fFormat.FrameBytes());}

    /* From the node's thread. */
    void Fill(uint8_t *out, size_t frames);
    void SetLead(bigtime_t lead) {fLeadUs.store(lead);}
};


//#pragma mark - AudioProducerNode

AudioProducerNode::AudioProducerNode(const char *name, MediaAudio *owner):
    BMediaNode(name),
    BBufferProducer(B_MEDIA_RAW_AUDIO),
    BMediaEventLooper(),
    fOwner(owner)
{
    fOutput.format.type = B_MEDIA_RAW_AUDIO;
    fOutput.format.u.raw_audio = media_multi_audio_format::wildcard;
    fWanted = media_multi_audio_format::wildcard;
}


AudioProducerNode::~AudioProducerNode()
{
    Quit();
    delete fBufferGroup;
}


BMediaAddOn *AudioProducerNode::AddOn(int32 *internal_id) const
{
    (void)internal_id;
    return nullptr;
}


status_t AudioProducerNode::HandleMessage(int32 message, const void *data,
                                          size_t size)
{
    (void)message;
    (void)data;
    (void)size;
    return B_ERROR;
}


void AudioProducerNode::NodeRegistered()
{
    SetPriority(B_URGENT_PRIORITY);
    fOutput.destination = media_destination::null;
    fOutput.source.port = ControlPort();
    fOutput.source.id = 0;
    fOutput.node = Node();
    strlcpy(fOutput.name, Name(), sizeof(fOutput.name));
    Run();
}


size_t AudioProducerNode::BufferFrames() const
{
    const media_multi_audio_format &f = fOutput.format.u.raw_audio;
    size_t frame_bytes = (f.format & media_raw_audio_format::B_AUDIO_SIZE_MASK) *
                         f.channel_count;
    return frame_bytes != 0 ? f.buffer_size / frame_bytes : 0;
}


/* Events are handled early enough for a buffer to get through everything
   downstream; the guest must be ahead of that, and of the buffer being
   filled. */
void AudioProducerNode::UpdateLatency()
{
    SetEventLatency(fLatency + fInternalLatency);
    fOwner->SetLead(EventLatency() + SchedulingLatency() + BufferDuration() +
                    MEDIA_LEAD_MARGIN_US);
}


status_t AudioProducerNode::FormatSuggestionRequested(media_type type,
                                                      int32 quality,
                                                      media_format *format)
{
    (void)quality;
    if (type != B_MEDIA_RAW_AUDIO && type != B_MEDIA_UNKNOWN_TYPE) {
        return B_MEDIA_BAD_FORMAT;
    }
    format->type = B_MEDIA_RAW_AUDIO;
    format->u.raw_audio = fWanted;
    return B_OK;
}


status_t AudioProducerNode::FormatProposal(const media_source &output,
                                           media_format *format)
{
    if (output != fOutput.source) {
        return B_MEDIA_BAD_SOURCE;
    }
    if (format->type == B_MEDIA_UNKNOWN_TYPE) {
        format->type = B_MEDIA_RAW_AUDIO;
    }
    if (format->type != B_MEDIA_RAW_AUDIO) {
        return B_MEDIA_BAD_FORMAT;
    }
    return B_OK;
}


status_t AudioProducerNode::FormatChangeRequested(
    const media_source &source, const media_destination &destination,
    media_format *format, int32 *deprecated)
{
    (void)source;
    (void)destination;
    (void)format;
    (void)deprecated;
    /* The format is the guest's, which nobody downstream may change. */
    return B_ERROR;
}


status_t AudioProducerNode::GetNextOutput(int32 *cookie, media_output *output)
{
    if (*cookie != 0) {
        return B_BAD_INDEX;
    }
    *output = fOutput;
    *cookie += 1;
    return B_OK;
}


status_t AudioProducerNode::DisposeOutputCookie(int32 cookie)
{
    (void)cookie;
    return B_OK;
}


status_t AudioProducerNode::SetBufferGroup(const media_source &source,
                                           BBufferGroup *group)
{
    if (source != fOutput.source) {
        return B_MEDIA_BAD_SOURCE;
    }
    if (group == fBufferGroup) {
        return B_OK;
    }
    /* waits for every buffer of the old group to come back */
    delete fBufferGroup;
    fBufferGroup = nullptr;
    if (group != nullptr) {
        fBufferGroup = group;
        return B_OK;
    }
    return AllocateBuffers();
}


status_t AudioProducerNode::GetLatency(bigtime_t *latency)
{
    *latency = EventLatency() + SchedulingLatency();
    return B_OK;
}


status_t AudioProducerNode::PrepareToConnect(const media_source &what,
                                             const media_destination &where,
                                             media_format *format,
                                             media_source *source, char *name)
{
    if (what != fOutput.source) {
        return B_MEDIA_BAD_SOURCE;
    }
    if (fOutput.destination != media_destination::null) {
        return B_MEDIA_ALREADY_CONNECTED;
    }
    if (format->type != B_MEDIA_UNKNOWN_TYPE &&
        format->type != B_MEDIA_RAW_AUDIO) {
        return B_MEDIA_BAD_FORMAT;
    }

    /* Whatever the consumer left open is filled in from the guest's format,
       and anything it changed is refused. */
    media_format wanted;
    wanted.type = B_MEDIA_RAW_AUDIO;
    wanted.u.raw_audio = fWanted;
    format->SpecializeTo(&wanted);
    const media_multi_audio_format &f = format->u.raw_audio;
    if (f.frame_rate != fWanted.frame_rate ||
        f.channel_count != fWanted.channel_count ||
        f.format != fWanted.format || f.buffer_size != fWanted.buffer_size) {
        return B_MEDIA_BAD_FORMAT;
    }

    fOutput.destination = where;
    fOutput.format = *format;
    *source = fOutput.source;
    strlcpy(name, Name(), B_MEDIA_NAME_LENGTH);
    return B_OK;
}


void AudioProducerNode::Connect(status_t error, const media_source &source,
                                const media_destination &destination,
                                const media_format &format, char *name)
{
    if (source != fOutput.source) {
        return;
    }
    if (error != B_OK) {
        fOutput.destination = media_destination::null;
        fOutput.format.u.raw_audio = media_multi_audio_format::wildcard;
        return;
    }
    fOutput.destination = destination;
    fOutput.format = format;
    strlcpy(name, Name(), B_MEDIA_NAME_LENGTH);

    media_node_id id;
    FindLatencyFor(fOutput.destination, &fLatency, &id);
    SetBufferDuration((bigtime_t)BufferFrames() * 1000000 /
                      (bigtime_t)fOutput.format.u.raw_audio.frame_rate);
    fInternalLatency = BufferDuration() * 3 / 4;
    UpdateLatency();

    if (fBufferGroup == nullptr) {
        AllocateBuffers();
    }
}


void AudioProducerNode::Disconnect(const media_source &what,
                                   const media_destination &where)
{
    if (what != fOutput.source || where != fOutput.destination) {
        return;
    }
    fOutput.destination = media_destination::null;
    fOutput.format.u.raw_audio = media_multi_audio_format::wildcard;
    delete fBufferGroup;
    fBufferGroup = nullptr;
}


void AudioProducerNode::LateNoticeReceived(const media_source &what,
                                           bigtime_t how_much,
                                           bigtime_t performance_time)
{
    (void)performance_time;
    if (what != fOutput.source) {
        return;
    }
    /* Start sending earlier, within reason. */
    fInternalLatency += how_much;
    if (fInternalLatency > 30000) {
        fInternalLatency = 30000;
    }
    UpdateLatency();
}


void AudioProducerNode::EnableOutput(const media_source &what, bool enabled,
                                     int32 *deprecated)
{
    (void)deprecated;
    if (what == fOutput.source) {
        fOutputEnabled = enabled;
    }
}


void AudioProducerNode::LatencyChanged(const media_source &source,
                                       const media_destination &destination,
                                       bigtime_t new_latency, uint32 flags)
{
    (void)flags;
    if (source == fOutput.source && destination == fOutput.destination) {
        fLatency = new_latency;
        UpdateLatency();
    }
}


status_t AudioProducerNode::AllocateBuffers()
{
    /* enough to span the latency downstream, and one more */
    int32 count = (int32)(fLatency / BufferDuration()) + 2;
    if (count < 3) {
        count = 3;
    }
    fBufferGroup = new BBufferGroup(fOutput.format.u.raw_audio.buffer_size,
                                    count);
    return fBufferGroup->InitCheck();
}


void AudioProducerNode::SendNewBuffer(const media_timed_event *event)
{
    if (RunState() != B_STARTED ||
        fOutput.destination == media_destination::null) {
        return;
    }

    size_t frames = BufferFrames();
    if (fOutputEnabled && fBufferGroup != nullptr) {
        BBuffer *buffer = fBufferGroup->RequestBuffer(
            fOutput.format.u.raw_audio.buffer_size, BufferDuration() / 2);
        if (buffer != nullptr) {
            fOwner->Fill((uint8_t *)buffer->Data(), frames);
            media_header *header = buffer->Header();
            header->type = B_MEDIA_RAW_AUDIO;
            header->size_used = fOutput.format.u.raw_audio.buffer_size;
            header->time_source = TimeSource()->ID();
            header->start_time = event->event_time;
            if (SendBuffer(buffer, fOutput.source,
                           fOutput.destination) != B_OK) {
                buffer->Recycle();
            }
        } else {
            /* the frames are due whether or not they could be sent */
            fOwner->Fill(nullptr, frames);
        }
    } else {
        fOwner->Fill(nullptr, frames);
    }

    /* The next buffer plays when this one ends, counted in frames so that
       no rounding accumulates. */
    fFramesSent += frames;
    bigtime_t next = fStartTime +
                     (bigtime_t)(fFramesSent * 1000000 /
                                 (uint64)fOutput.format.u.raw_audio.frame_rate);
    media_timed_event next_event(next, SEND_BUFFER_EVENT);
    EventQueue()->AddEvent(next_event);
}


void AudioProducerNode::HandleEvent(const media_timed_event *event,
                                    bigtime_t lateness, bool real_time_event)
{
    (void)lateness;
    (void)real_time_event;

    switch (event->type) {
    case BTimedEventQueue::B_START:
        if (RunState() != B_STARTED) {
            fFramesSent = 0;
            fStartTime = event->event_time;
            media_timed_event first(event->event_time, SEND_BUFFER_EVENT);
            EventQueue()->AddEvent(first);
        }
        break;
    case BTimedEventQueue::B_STOP:
        EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
                                  SEND_BUFFER_EVENT);
        break;
    case SEND_BUFFER_EVENT:
        SendNewBuffer(event);
        break;
    default:
        break;
    }
}


//#pragma mark - MediaAudio

MediaAudio::MediaAudio(EventLoop &loop, const AudioSettings &settings):
    PacedAudio(loop, settings.direction),
    fLatencyMs(settings.latency_ms)
{
    if (settings.device != nullptr) {
        fprintf(stderr, "audio: output devices are not chosen by name on "
                "this host; playing through the system mixer\n");
    }
}


MediaAudio::~MediaAudio()
{
    Stop();
    DisconnectNode();
    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (roster != nullptr && fHaveMixer) {
        roster->ReleaseNode(fMixer);
    }
    /* The node goes once the last reference does, which is this one. */
    if (fNode != nullptr) {
        fNode->Release();
    }
}


bool MediaAudio::Init()
{
    status_t err;
    BMediaRoster *roster = BMediaRoster::Roster(&err);

    if (roster == nullptr ||
        (err = roster->GetAudioMixer(&fMixer)) != B_OK) {
        fprintf(stderr, "audio: no system mixer: %s\n", strerror(err));
        return false;
    }
    fHaveMixer = true;

    fNode = new AudioProducerNode("TinyEMU", this);
    media_node time_source;
    if ((err = roster->RegisterNode(fNode)) != B_OK ||
        (err = roster->GetTimeSource(&time_source)) != B_OK ||
        (err = roster->SetTimeSourceFor(fNode->Node().node,
                                        time_source.node)) != B_OK ||
        (err = roster->SetRunModeNode(fNode->Node(),
                                      BMediaNode::B_INCREASE_LATENCY)) !=
            B_OK) {
        fprintf(stderr, "audio: could not set up the node: %s\n",
                strerror(err));
        return false;
    }

    /* The guest is offered what the mixer plays at, so that it is only
       mixed, not resampled. */
    media_output mixer_out;
    int32 count = 0;
    fCaps.any_rate = true;
    if (roster->GetConnectedOutputsFor(fMixer, &mixer_out, 1, &count) ==
            B_OK &&
        count > 0 && mixer_out.format.type == B_MEDIA_RAW_AUDIO &&
        mixer_out.format.u.raw_audio.frame_rate > 0) {
        fCaps.rate = (uint32_t)mixer_out.format.u.raw_audio.frame_rate;
        fCaps.channels = mixer_out.format.u.raw_audio.channel_count;
    }
    return true;
}


media_multi_audio_format MediaAudio::MediaFormat() const
{
    media_multi_audio_format f = media_multi_audio_format::wildcard;
    size_t frame_bytes = fFormat.FrameBytes();

    f.frame_rate = fFormat.rate;
    f.channel_count = fFormat.channels;
    switch (fFormat.ContainerBytes()) {
    case 1:
        f.format = media_raw_audio_format::B_AUDIO_CHAR;
        break;
    case 2:
        f.format = media_raw_audio_format::B_AUDIO_SHORT;
        break;
    default:
        f.format = media_raw_audio_format::B_AUDIO_INT;
        break;
    }
    f.valid_bits = fFormat.bits;
    f.byte_order = B_MEDIA_LITTLE_ENDIAN;

    /* The roster's usual size, but no longer than half the latency asked
       for, and never so long that the guest's buffers are taken in
       lumps. */
    size_t frames = BMediaRoster::Roster()->AudioBufferSizeFor(
                        f.channel_count, f.format, f.frame_rate) /
                    frame_bytes;
    size_t max_frames = (size_t)fFormat.rate *
                        (fLatencyMs * 500 < MEDIA_MAX_BUFFER_US
                             ? fLatencyMs * 500 : MEDIA_MAX_BUFFER_US) /
                        1000000;
    if (frames == 0 || frames > max_frames) {
        frames = max_frames;
    }
    if (frames < 16) {
        frames = 16;
    }
    f.buffer_size = frames * frame_bytes;
    return f;
}


bool MediaAudio::ConnectNode(const media_multi_audio_format &format)
{
    BMediaRoster *roster = BMediaRoster::Roster();
    media_output out;
    media_input in;
    int32 count;
    status_t err;

    fNode->SetWanted(format);
    if ((err = roster->GetFreeInputsFor(fMixer, &in, 1, &count,
                                        B_MEDIA_RAW_AUDIO)) != B_OK ||
        count < 1 ||
        (err = roster->GetFreeOutputsFor(fNode->Node(), &out, 1, &count,
                                         B_MEDIA_RAW_AUDIO)) != B_OK ||
        count < 1) {
        fprintf(stderr, "audio: the mixer has no free input\n");
        return false;
    }
    media_format try_format;
    try_format.type = B_MEDIA_RAW_AUDIO;
    try_format.u.raw_audio = format;
    if ((err = roster->Connect(out.source, in.destination, &try_format,
                               &fOutput, &fInput)) != B_OK) {
        fprintf(stderr, "audio: could not connect to the mixer: %s\n",
                strerror(err));
        return false;
    }
    fConnected = true;
    fConnectedFormat = format;
    return true;
}


void MediaAudio::DisconnectNode()
{
    if (fConnected) {
        BMediaRoster::Roster()->Disconnect(fOutput, fInput);
        fConnected = false;
    }
}


uint64_t MediaAudio::ClockFrames()
{
    bigtime_t start = fStartTime.load();
    if (start == 0) {
        return 0;
    }
    bigtime_t ahead = fNode->TimeSource()->Now() - start + fLeadUs.load();
    if (ahead <= 0) {
        return 0;
    }
    return (uint64_t)ahead * fFormat.rate / 1000000;
}


bool MediaAudio::StartStream()
{
    BMediaRoster *roster = BMediaRoster::Roster();
    media_multi_audio_format format = MediaFormat();

    if (fConnected &&
        (fConnectedFormat.frame_rate != format.frame_rate ||
         fConnectedFormat.channel_count != format.channel_count ||
         fConnectedFormat.format != format.format ||
         fConnectedFormat.valid_bits != format.valid_bits ||
         fConnectedFormat.buffer_size != format.buffer_size)) {
        DisconnectNode();
    }
    if (!fConnected && !ConnectNode(format)) {
        return false;
    }

    size_t capacity = (size_t)((fLeadUs.load() + MEDIA_RING_MS * 1000LL) *
                           fFormat.rate / 1000000);
    fRing.Reset(fFormat.FrameBytes(), capacity);
    fDebt = 0;
    fUnderruns = 0;

    BTimeSource *ts = fNode->TimeSource();
    if (!ts->IsRunning()) {
        roster->StartTimeSource(ts->Node(), ts->RealTime());
    }
    bigtime_t latency = 0;
    roster->GetLatencyFor(fNode->Node(), &latency);
    bigtime_t start = ts->Now() + latency + 5000;
    fStartTime.store(start);
    if (roster->StartNode(fNode->Node(), start) != B_OK) {
        fStartTime.store(0);
        return false;
    }
    return true;
}


void MediaAudio::StopStream()
{
    /* Waits until the node has stopped, after which it no longer reads the
       ring. */
    BMediaRoster::Roster()->StopNode(fNode->Node(), 0, true);
    fStartTime.store(0);
    if (fUnderruns != 0) {
        fprintf(stderr, "audio: the output ran dry %u times\n", fUnderruns);
    }
}


void MediaAudio::Write(const uint8_t *buf, size_t frames)
{
    if (Running()) {
        fRing.Write(buf, frames);
    }
}


/* A ring that runs dry is made up for with silence, and as many frames are
   skipped once it refills, so that the latency stays what it was. A null
   buffer takes the frames without keeping them. */
void MediaAudio::Fill(uint8_t *out, size_t frames)
{
    size_t frame_bytes = fFormat.FrameBytes();

    if (fDebt > 0) {
        size_t avail = fRing.Available();
        fDebt -= fRing.Read(nullptr, fDebt < avail ? fDebt : avail);
    }
    size_t got = fRing.Read(out, frames);
    if (got < frames) {
        if (out != nullptr) {
            memset(out + got * frame_bytes, 0, (frames - got) * frame_bytes);
        }
        fDebt += frames - got;
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
    auto audio = std::make_unique<MediaAudio>(loop, settings);
    if (!audio->Init()) {
        return nullptr;
    }
    return audio;
}
