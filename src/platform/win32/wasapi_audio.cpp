/*
 * Windows audio through WASAPI
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
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "host_time.h"
#include "paced_audio.h"
#include "platform_backends.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* Spelled out rather than taken from the SDK's libraries, which not every
   toolchain has all of. */
static const CLSID kClsidMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c,
    {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID kIidMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35,
    {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID kIidAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32,
    {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID kIidAudioClient3 = {0x7ed4ee07, 0x8e67, 0x4cd4,
    {0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad, 0x42}};
static const IID kIidAudioRenderClient = {0xf294acfc, 0x3146, 0x4483,
    {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const IID kIidMMNotificationClient = {0x7991eec9, 0x7e89, 0x4d85,
    {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const IID kIidUnknown = {0x00000000, 0x0000, 0x0000,
    {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd,
    {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* The least the guest is kept ahead of the endpoint, whatever the latency
   asked for: enough to ride out a late tick of the event loop. */
#define WASAPI_MIN_LEAD_MS 4
/* The least the endpoint's own buffer is kept filled to. */
#define WASAPI_MIN_PADDING_MS 3
/* An endpoint that signals nothing for this long is opened again. */
#define WASAPI_STALL_MS 200
/* How long the ring can hold, beyond the lead. */
#define WASAPI_RING_MS 250


template <typename T>
static void com_release(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}


static std::wstring widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    }
    return w;
}


static std::wstring lower(std::wstring s)
{
    for (auto &c : s) {
        c = towlower(c);
    }
    return s;
}


/* The render endpoint whose name contains 'name', or the default one when
   'name' is empty or matches nothing. */
static IMMDevice *find_device(IMMDeviceEnumerator *en, const std::wstring &name,
                              bool *warned)
{
    IMMDevice *dev = nullptr;

    if (!name.empty()) {
        IMMDeviceCollection *coll = nullptr;
        UINT count = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                             &coll)) &&
            SUCCEEDED(coll->GetCount(&count))) {
            std::wstring want = lower(name);
            for (UINT i = 0; i < count && dev == nullptr; i++) {
                IMMDevice *d = nullptr;
                IPropertyStore *props = nullptr;
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(coll->Item(i, &d)) &&
                    SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) &&
                    SUCCEEDED(props->GetValue(kPkeyFriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR &&
                    lower(pv.pwszVal).find(want) != std::wstring::npos) {
                    dev = d;
                    d = nullptr;
                }
                PropVariantClear(&pv);
                com_release(props);
                com_release(d);
            }
        }
        com_release(coll);
        if (dev != nullptr) {
            return dev;
        }
        if (!*warned) {
            fprintf(stderr, "audio: no output device matches '%ls', using the "
                    "default one\n", name.c_str());
            *warned = true;
        }
    }
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        return nullptr;
    }
    return dev;
}


/* How samples leave the ring for the endpoint. */
typedef enum {
    OUT_COPY,
    OUT_FLOAT32,
    OUT_INT16,
    OUT_INT32,
} OutKindEnum;


class WasapiAudio;

/* Tells the render thread that the default device moved, so that a stream
   following it moves too. */
class WasapiNotifier final: public IMMNotificationClient {
private:
    WasapiAudio *fOwner;

public:
    explicit WasapiNotifier(WasapiAudio *owner): fOwner(owner) {}
    virtual ~WasapiNotifier() = default;

    /* Lives exactly as long as its owner, so it counts no references. */
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (IsEqualIID(riid, kIidUnknown) ||
            IsEqualIID(riid, kIidMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient *>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,
        ERole role, LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,
        DWORD state) override {(void)id; (void)state; return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override
        {(void)id; return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override
        {(void)id; return S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,
        const PROPERTYKEY key) override {(void)id; (void)key; return S_OK;}
};


/* One render stream. The guest's frames go into a ring from the event loop;
   a thread of its own takes them out as the endpoint asks for them, and
   what it has handed over is the clock the device runs by. */
class WasapiAudio final: public PacedAudio {
private:
    std::wstring fDeviceName;
    int fLatencyMs;
    AudioCaps fCaps;
    WasapiNotifier fNotifier {this};

    std::thread fThread;
    std::atomic<bool> fQuit {false};
    std::atomic<bool> fReopen {false};
    HANDLE fWakeEvent = nullptr;
    HANDLE fBufferEvent = nullptr;
    AudioRing fRing;

    /* The frames handed to the endpoint and when, for the device thread:
       a sequence lock, odd while the render thread updates them. */
    std::atomic<uint32_t> fClockSeq {0};
    std::atomic<uint64_t> fClockFrames {0};
    std::atomic<uint64_t> fClockUs {0};
    std::atomic<uint64_t> fMaxExtrapolate {0};

    /* the render thread's own */
    IAudioClient *fClient = nullptr;
    IAudioRenderClient *fRender = nullptr;
    OutKindEnum fOutKind = OUT_COPY;
    int fOutFrameBytes = 0;
    UINT32 fTargetPadding = 0;
    uint64_t fConsumed = 0;
    size_t fLeadLeft = 0;
    bool fLeadDone = false;
    size_t fDebt = 0;
    uint64_t fFallbackUs = 0;
    unsigned fUnderruns = 0;
    bool fWarned = false;
    std::vector<uint8_t> fScratch;

    void ThreadMain();
    bool OpenClient(IMMDeviceEnumerator *en);
    void CloseClient();
    bool Render();
    void Fill(uint8_t *out, size_t frames);
    void Convert(const uint8_t *in, uint8_t *out, size_t frames);
    void FallbackTick();
    void Publish();

protected:
    uint64_t ClockFrames() override;
    bool StartStream() override;
    void StopStream() override;

public:
    WasapiAudio(EventLoop &loop, const AudioSettings &settings);
    ~WasapiAudio() override;

    void QueryCaps();
    void RequestReopen()
    {
        fReopen.store(true);
        SetEvent(fWakeEvent);
    }

    AudioCaps Caps() override {return fCaps;}
    void Write(const uint8_t *buf, size_t frames) override;
    void Read(uint8_t *buf, size_t frames) override
        {memset(buf, 0, frames * fFormat.FrameBytes());}
};


HRESULT STDMETHODCALLTYPE WasapiNotifier::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)id;
    if (flow == eRender && role == eConsole) {
        fOwner->RequestReopen();
    }
    return S_OK;
}


WasapiAudio::WasapiAudio(EventLoop &loop, const AudioSettings &settings):
    PacedAudio(loop, settings.direction),
    fLatencyMs(settings.latency_ms)
{
    if (settings.device != nullptr) {
        fDeviceName = widen(settings.device);
    }
    fWakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    fBufferEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}


WasapiAudio::~WasapiAudio()
{
    Stop();
    CloseHandle(fWakeEvent);
    CloseHandle(fBufferEvent);
}


/* What the endpoint runs at. Asked on a thread of its own, so that the COM
   apartment of whichever thread built the machine is left alone. */
void WasapiAudio::QueryCaps()
{
    std::thread([this]() {
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *client = nullptr;
        WAVEFORMATEX *mix = nullptr;

        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        fCaps.any_rate = true;
        if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr,
                                       CLSCTX_ALL, kIidMMDeviceEnumerator,
                                       (void **)&en)) &&
            (dev = find_device(en, fDeviceName, &fWarned)) != nullptr &&
            SUCCEEDED(dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                                    (void **)&client)) &&
            SUCCEEDED(client->GetMixFormat(&mix))) {
            fCaps.rate = mix->nSamplesPerSec;
            fCaps.channels = mix->nChannels;
        } else {
            fprintf(stderr, "audio: no output device yet\n");
        }
        if (mix != nullptr) {
            CoTaskMemFree(mix);
        }
        com_release(client);
        com_release(dev);
        com_release(en);
        CoUninitialize();
    }).join();
}


//#pragma mark - device side

uint64_t WasapiAudio::ClockFrames()
{
    uint64_t frames, us;
    uint32_t seq;

    do {
        seq = fClockSeq.load(std::memory_order_acquire);
        frames = fClockFrames.load(std::memory_order_relaxed);
        us = fClockUs.load(std::memory_order_relaxed);
    } while ((seq & 1) != 0 ||
             seq != fClockSeq.load(std::memory_order_acquire));

    if (us == 0) {
        return frames;
    }
    /* Between two buffers the endpoint plays on at its rate; past two
       periods without word from it, the guest waits. */
    uint64_t now = host_monotonic_us();
    uint64_t ahead = now > us ? (now - us) * fFormat.rate / 1000000 : 0;
    uint64_t max_ahead = fMaxExtrapolate.load(std::memory_order_relaxed);
    return frames + (ahead < max_ahead ? ahead : max_ahead);
}


bool WasapiAudio::StartStream()
{
    size_t lead = (size_t)fFormat.rate * WASAPI_MIN_LEAD_MS / 1000;

    fRing.Reset(fFormat.FrameBytes(),
                lead + (size_t)fFormat.rate * (fLatencyMs + WASAPI_RING_MS) /
                           1000);
    fClockSeq.store(0);
    fClockFrames.store(0);
    fClockUs.store(0);
    fMaxExtrapolate.store((uint64_t)fFormat.rate * 20 / 1000);
    fConsumed = 0;
    fLeadDone = false;
    fDebt = 0;
    fFallbackUs = 0;
    fUnderruns = 0;
    fQuit.store(false);
    fReopen.store(false);
    ResetEvent(fWakeEvent);
    fThread = std::thread([this]() {ThreadMain();});
    return true;
}


void WasapiAudio::StopStream()
{
    fQuit.store(true);
    SetEvent(fWakeEvent);
    fThread.join();
    if (fUnderruns != 0) {
        fprintf(stderr, "audio: the output ran dry %u times\n", fUnderruns);
    }
}


void WasapiAudio::Write(const uint8_t *buf, size_t frames)
{
    if (Running()) {
        fRing.Write(buf, frames);
    }
}


//#pragma mark - render thread

void WasapiAudio::Publish()
{
    uint32_t seq = fClockSeq.load(std::memory_order_relaxed);
    fClockSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    fClockFrames.store(fConsumed, std::memory_order_relaxed);
    fClockUs.store(host_monotonic_us(), std::memory_order_relaxed);
    fClockSeq.store(seq + 2, std::memory_order_release);
}


void WasapiAudio::ThreadMain()
{
    IMMDeviceEnumerator *en = nullptr;
    DWORD task_index = 0;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMMDeviceEnumerator, (void **)&en))) {
        en = nullptr;
    }
    bool follow_default = en != nullptr && fDeviceName.empty();
    if (follow_default) {
        en->RegisterEndpointNotificationCallback(&fNotifier);
    }

    while (!fQuit.load()) {
        if (fClient == nullptr && (en == nullptr || !OpenClient(en))) {
            /* No endpoint to play on: the guest keeps its time by the host's
               clock until there is one. */
            FallbackTick();
            WaitForSingleObject(fWakeEvent, 10);
            fReopen.store(false);
            continue;
        }
        HANDLE handles[2] = {fBufferEvent, fWakeEvent};
        DWORD r = WaitForMultipleObjects(2, handles, FALSE, WASAPI_STALL_MS);
        if (fQuit.load()) {
            break;
        }
        if (r == WAIT_TIMEOUT || fReopen.exchange(false) || !Render()) {
            CloseClient();
        }
    }

    CloseClient();
    if (follow_default) {
        en->UnregisterEndpointNotificationCallback(&fNotifier);
    }
    com_release(en);
    if (mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    CoUninitialize();
}


bool WasapiAudio::OpenClient(IMMDeviceEnumerator *en)
{
    IMMDevice *dev = find_device(en, fDeviceName, &fWarned);
    WAVEFORMATEX *mix = nullptr;
    WAVEFORMATEXTENSIBLE ext = {};
    const WAVEFORMATEX *wfx;
    UINT32 period = 0;
    UINT32 buffer_frames = 0;
    bool ok = false;

    if (dev == nullptr ||
        FAILED(dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                             (void **)&fClient)) ||
        FAILED(fClient->GetMixFormat(&mix))) {
        goto out;
    }

    /* At the endpoint's own rate and width the samples only need widening,
       which is done here, and the engine's smallest period may be had. Any
       other format the engine converts. */
    fOutKind = OUT_COPY;
    if (mix->nSamplesPerSec == fFormat.rate &&
        mix->nChannels == fFormat.channels) {
        const GUID *sub = nullptr;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mix->cbSize >= 22) {
            sub = &((WAVEFORMATEXTENSIBLE *)mix)->SubFormat;
        }
        bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (sub != nullptr && IsEqualGUID(*sub, kSubtypeFloat));
        bool is_pcm = mix->wFormatTag == WAVE_FORMAT_PCM ||
                      (sub != nullptr && IsEqualGUID(*sub, kSubtypePcm));
        if (is_float && mix->wBitsPerSample == 32) {
            fOutKind = OUT_FLOAT32;
        } else if (is_pcm && mix->wBitsPerSample == 16) {
            fOutKind = OUT_INT16;
        } else if (is_pcm && mix->wBitsPerSample == 32) {
            fOutKind = OUT_INT32;
        }
    }

    if (fOutKind != OUT_COPY) {
        wfx = mix;
        IAudioClient3 *client3 = nullptr;
        if (SUCCEEDED(fClient->QueryInterface(kIidAudioClient3,
                                              (void **)&client3))) {
            UINT32 def_period, fundamental, min_period, max_period;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                    mix, &def_period, &fundamental, &min_period,
                    &max_period)) &&
                SUCCEEDED(client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, min_period, mix,
                    nullptr))) {
                period = min_period;
            }
            com_release(client3);
        }
        if (period == 0) {
            /* A client whose initialization failed cannot be tried again. */
            com_release(fClient);
            if (FAILED(dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                                     (void **)&fClient)) ||
                FAILED(fClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           0, 0, wfx, nullptr))) {
                goto out;
            }
        }
        fOutFrameBytes = mix->nBlockAlign;
    } else {
        /* A sample of 8 bits is unsigned in a WAVEFORMAT, so those go out
           widened to 16. */
        int bits = fFormat.bits < 16 ? 16 : fFormat.bits;
        int container = bits <= 16 ? 2 : 4;
        if (fFormat.bits < 16) {
            fOutKind = OUT_INT16;
        }
        ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        ext.Format.nChannels = fFormat.channels;
        ext.Format.nSamplesPerSec = fFormat.rate;
        ext.Format.wBitsPerSample = container * 8;
        ext.Format.nBlockAlign = container * fFormat.channels;
        ext.Format.nAvgBytesPerSec = fFormat.rate * ext.Format.nBlockAlign;
        ext.Format.cbSize = 22;
        ext.Samples.wValidBitsPerSample = bits;
        ext.dwChannelMask = fFormat.channels == 1 ? SPEAKER_FRONT_CENTER
                            : fFormat.channels == 2
                                ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                : (1u << fFormat.channels) - 1;
        ext.SubFormat = kSubtypePcm;
        wfx = &ext.Format;
        if (FAILED(fClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, wfx, nullptr))) {
            goto out;
        }
        fOutFrameBytes = ext.Format.nBlockAlign;
    }

    if (period == 0) {
        REFERENCE_TIME def_period = 0, min_period = 0;
        fClient->GetDevicePeriod(&def_period, &min_period);
        period = (UINT32)(def_period * fFormat.rate / 10000000);
    }
    if (FAILED(fClient->GetBufferSize(&buffer_frames)) ||
        FAILED(fClient->SetEventHandle(fBufferEvent)) ||
        FAILED(fClient->GetService(kIidAudioRenderClient,
                                   (void **)&fRender))) {
        goto out;
    }

    /* Two periods in the endpoint's buffer, so that one late wakeup is not
       heard; the rest of the latency asked for is the guest's lead. */
    {
        UINT32 min_padding = fFormat.rate * WASAPI_MIN_PADDING_MS / 1000;
        fTargetPadding = 2 * period > min_padding ? 2 * period : min_padding;
        if (fTargetPadding > buffer_frames) {
            fTargetPadding = buffer_frames;
        }
        if (!fLeadDone) {
            size_t lead = (size_t)fFormat.rate * fLatencyMs / 1000;
            size_t min_lead = (size_t)fFormat.rate * WASAPI_MIN_LEAD_MS / 1000;
            lead = lead > fTargetPadding ? lead - fTargetPadding : 0;
            fLeadLeft = lead > min_lead ? lead : min_lead;
            fLeadDone = true;
        }
        fMaxExtrapolate.store(2 * period);
    }

    /* The buffer starts out silent rather than empty. */
    {
        BYTE *data;
        if (SUCCEEDED(fRender->GetBuffer(fTargetPadding, &data))) {
            fRender->ReleaseBuffer(fTargetPadding, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }
    if (FAILED(fClient->Start())) {
        goto out;
    }
    fFallbackUs = 0;
    ok = true;

out:
    if (mix != nullptr) {
        CoTaskMemFree(mix);
    }
    com_release(dev);
    if (!ok) {
        CloseClient();
    }
    return ok;
}


void WasapiAudio::CloseClient()
{
    if (fClient != nullptr) {
        fClient->Stop();
    }
    com_release(fRender);
    com_release(fClient);
}


bool WasapiAudio::Render()
{
    UINT32 padding;
    BYTE *data;

    if (FAILED(fClient->GetCurrentPadding(&padding))) {
        return false;
    }
    if (padding >= fTargetPadding) {
        return true;
    }
    UINT32 frames = fTargetPadding - padding;
    if (FAILED(fRender->GetBuffer(frames, &data))) {
        return false;
    }
    Fill(data, frames);
    fRender->ReleaseBuffer(frames, 0);
    fConsumed += frames;
    Publish();
    return true;
}


/* The guest's lead first, then the ring. A ring that runs dry is made up for
   with silence, and as many frames are skipped once it refills, so that the
   latency stays what it was. */
void WasapiAudio::Fill(uint8_t *out, size_t frames)
{
    size_t done = 0;

    if (fLeadLeft > 0) {
        size_t n = fLeadLeft < frames ? fLeadLeft : frames;
        memset(out, 0, n * fOutFrameBytes);
        fLeadLeft -= n;
        done = n;
    }
    if (fDebt > 0) {
        size_t avail = fRing.Available();
        fDebt -= fRing.Read(nullptr, fDebt < avail ? fDebt : avail);
    }

    size_t in_bytes = fFormat.FrameBytes();
    while (done < frames) {
        size_t n = frames - done;
        if (n > 256) {
            n = 256;
        }
        fScratch.resize(n * in_bytes);
        n = fRing.Read(fScratch.data(), n);
        if (n == 0) {
            break;
        }
        Convert(fScratch.data(), out + done * fOutFrameBytes, n);
        done += n;
    }
    if (done < frames) {
        memset(out + done * fOutFrameBytes, 0, (frames - done) * fOutFrameBytes);
        fDebt += frames - done;
        fUnderruns++;
    }
}


void WasapiAudio::Convert(const uint8_t *in, uint8_t *out, size_t frames)
{
    size_t samples = frames * fFormat.channels;
    int in_bytes = fFormat.ContainerBytes();

    switch (fOutKind) {
    case OUT_COPY:
        memcpy(out, in, frames * fFormat.FrameBytes());
        break;
    case OUT_FLOAT32:
        for (size_t i = 0; i < samples; i++) {
            float v = (float)audio_sample_get(in + i * in_bytes, in_bytes) *
                      (1.0f / 2147483648.0f);
            memcpy(out + i * 4, &v, 4);
        }
        break;
    case OUT_INT16:
        for (size_t i = 0; i < samples; i++) {
            int16_t v = (int16_t)(audio_sample_get(in + i * in_bytes,
                                                   in_bytes) >> 16);
            memcpy(out + i * 2, &v, 2);
        }
        break;
    case OUT_INT32:
        for (size_t i = 0; i < samples; i++) {
            int32_t v = audio_sample_get(in + i * in_bytes, in_bytes);
            memcpy(out + i * 4, &v, 4);
        }
        break;
    }
}


/* Keeps time by the host's clock while there is no endpoint, dropping what
   the guest plays. */
void WasapiAudio::FallbackTick()
{
    uint64_t now = host_monotonic_us();

    if (fFallbackUs == 0) {
        fFallbackUs = now;
        return;
    }
    uint64_t frames = (now - fFallbackUs) * fFormat.rate / 1000000;
    if (frames == 0) {
        return;
    }
    fFallbackUs += frames * 1000000 / fFormat.rate;
    size_t avail = fRing.Available();
    fRing.Read(nullptr, frames < avail ? frames : avail);
    fConsumed += frames;
    Publish();
}


std::unique_ptr<HostAudio> host_audio_open(EventLoop &loop,
                                           const AudioSettings &settings)
{
    if (settings.direction != AUDIO_RENDER) {
        fprintf(stderr, "audio: capture from the host is not supported "
                "yet\n");
        return nullptr;
    }
    auto audio = std::make_unique<WasapiAudio>(loop, settings);
    audio->QueryCaps();
    return audio;
}
