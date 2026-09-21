/*
 * WAV file audio back end
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
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "paced_audio.h"
#include "platform_backends.h"

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_EXTENSIBLE 0xfffe

/* The header this writes: RIFF, a WAVEFORMATEXTENSIBLE, and the data chunk
   header. */
#define WAV_HEADER_SIZE 68
/* Where the two sizes the header carries sit. */
#define WAV_RIFF_SIZE_OFFSET 4
#define WAV_DATA_SIZE_OFFSET 64


static void wav_put16(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}


static void wav_put32(uint8_t *p, uint32_t v)
{
    wav_put16(p, v);
    wav_put16(p + 2, v >> 16);
}


static uint32_t wav_get16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}


static uint32_t wav_get32(const uint8_t *p)
{
    return wav_get16(p) | wav_get16(p + 2) << 16;
}


/* A WAV sample of 8 bits is unsigned, where every other size is signed. */
static void wav_flip_8bit(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] ^= 0x80;
    }
}


//#pragma mark - WavWriter

/* Records what the guest plays. The file takes the format of the first
   stream; a stream in another format starts the file over. */
class WavWriter final: public PacedAudio {
private:
    std::string fPath;
    FILE *fFile = nullptr;
    AudioFormat fFileFormat;
    bool fHaveHeader = false;
    uint64_t fDataBytes = 0;
    uint64_t fSyncedBytes = 0;
    std::vector<uint8_t> fScratch;

    void WriteHeader();
    void UpdateSizes();

protected:
    bool StartStream() override;
    void StopStream() override {UpdateSizes();}

public:
    WavWriter(EventLoop &loop, const char *path):
        PacedAudio(loop, AUDIO_RENDER), fPath(path) {}
    ~WavWriter() override;

    bool Open();

    AudioCaps Caps() override
    {
        AudioCaps caps;
        caps.any_rate = true;
        return caps;
    }

    void Write(const uint8_t *buf, size_t frames) override;
    void Read(uint8_t *buf, size_t frames) override
        {memset(buf, 0, frames * fFormat.FrameBytes());}
};


WavWriter::~WavWriter()
{
    if (fFile != nullptr) {
        UpdateSizes();
        fclose(fFile);
    }
}


bool WavWriter::Open()
{
    fFile = fopen(fPath.c_str(), "wb");
    if (fFile == nullptr) {
        fprintf(stderr, "%s: could not create\n", fPath.c_str());
        return false;
    }
    return true;
}


void WavWriter::WriteHeader()
{
    uint8_t h[WAV_HEADER_SIZE] = {};
    const AudioFormat &f = fFileFormat;
    int container_bits = f.ContainerBytes() * 8;
    uint32_t channel_mask = f.channels == 1 ? 0x4 : (1u << f.channels) - 1;

    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    wav_put32(h + 16, 40);
    wav_put16(h + 20, WAVE_FORMAT_EXTENSIBLE);
    wav_put16(h + 22, f.channels);
    wav_put32(h + 24, f.rate);
    wav_put32(h + 28, f.rate * f.FrameBytes());
    wav_put16(h + 32, f.FrameBytes());
    wav_put16(h + 34, container_bits);
    wav_put16(h + 36, 22);
    wav_put16(h + 38, f.bits);
    wav_put32(h + 40, channel_mask);
    /* KSDATAFORMAT_SUBTYPE_PCM */
    static const uint8_t kSubtypePcm[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71,
    };
    memcpy(h + 44, kSubtypePcm, 16);
    memcpy(h + 60, "data", 4);

    fseek(fFile, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), fFile);
    fDataBytes = 0;
    UpdateSizes();
}


void WavWriter::UpdateSizes()
{
    if (fFile == nullptr || !fHaveHeader) {
        return;
    }
    /* A file too long for the header says as much as it can. */
    uint64_t data = fDataBytes;
    fSyncedBytes = fDataBytes;
    if (data > 0xffffffffu - WAV_HEADER_SIZE) {
        data = 0xffffffffu - WAV_HEADER_SIZE;
    }
    uint8_t b[4];
    long pos = ftell(fFile);
    wav_put32(b, (uint32_t)data + WAV_HEADER_SIZE - 8);
    fseek(fFile, WAV_RIFF_SIZE_OFFSET, SEEK_SET);
    fwrite(b, 1, 4, fFile);
    wav_put32(b, (uint32_t)data);
    fseek(fFile, WAV_DATA_SIZE_OFFSET, SEEK_SET);
    fwrite(b, 1, 4, fFile);
    fseek(fFile, pos, SEEK_SET);
    fflush(fFile);
}


bool WavWriter::StartStream()
{
    if (fHaveHeader && fFormat != fFileFormat) {
        fprintf(stderr, "%s: the stream format changed, starting over\n",
                fPath.c_str());
        fFile = freopen(fPath.c_str(), "wb", fFile);
        fHaveHeader = false;
        if (fFile == nullptr) {
            fprintf(stderr, "%s: could not create\n", fPath.c_str());
            return false;
        }
    }
    if (!fHaveHeader) {
        fFileFormat = fFormat;
        fHaveHeader = true;
        WriteHeader();
    }
    return fFile != nullptr;
}


void WavWriter::Write(const uint8_t *buf, size_t frames)
{
    size_t len = frames * fFormat.FrameBytes();

    if (fFile == nullptr) {
        return;
    }
    if (fFormat.ContainerBytes() == 1) {
        fScratch.assign(buf, buf + len);
        wav_flip_8bit(fScratch.data(), len);
        buf = fScratch.data();
    }
    fwrite(buf, 1, len, fFile);
    fDataBytes += len;
    /* Once a second, so that an emulator that is killed rather than shut
       down still leaves a file that plays. */
    if (fDataBytes - fSyncedBytes >= fFormat.rate * fFormat.FrameBytes()) {
        UpdateSizes();
    }
}


//#pragma mark - WavReader

/* Plays a file to the guest, over and over when asked to. The whole file is
   read at once, so the event loop never waits on the disk. */
class WavReader final: public PacedAudio {
private:
    std::string fPath;
    bool fLoop;
    AudioFormat fFileFormat;
    std::vector<uint8_t> fData;
    size_t fFrames = 0;
    size_t fPos = 0;

protected:
    bool StartStream() override {return true;}
    void StopStream() override {}

public:
    WavReader(EventLoop &loop, const char *path, bool loop_file):
        PacedAudio(loop, AUDIO_CAPTURE), fPath(path), fLoop(loop_file) {}

    bool Open();

    AudioCaps Caps() override
    {
        AudioCaps caps;
        caps.rate = fFileFormat.rate;
        caps.channels = fFileFormat.channels;
        return caps;
    }

    void Write(const uint8_t *buf, size_t frames) override
        {(void)buf; (void)frames;}
    void Read(uint8_t *buf, size_t frames) override;
};


bool WavReader::Open()
{
    FILE *f = fopen(fPath.c_str(), "rb");
    if (f == nullptr) {
        fprintf(stderr, "%s: could not open\n", fPath.c_str());
        return false;
    }
    std::vector<uint8_t> file;
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        file.insert(file.end(), chunk, chunk + n);
    }
    fclose(f);

    if (file.size() < 12 || memcmp(&file[0], "RIFF", 4) != 0 ||
        memcmp(&file[8], "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a WAV file\n", fPath.c_str());
        return false;
    }
    bool have_fmt = false;
    for (size_t pos = 12; pos + 8 <= file.size();) {
        uint32_t size = wav_get32(&file[pos + 4]);
        const uint8_t *body = &file[pos + 8];
        size_t avail = file.size() - pos - 8;
        if (size > avail) {
            size = avail;
        }
        if (memcmp(&file[pos], "fmt ", 4) == 0 && size >= 16) {
            uint32_t tag = wav_get16(body);
            int container_bits = wav_get16(body + 14);
            fFileFormat.channels = wav_get16(body + 2);
            fFileFormat.rate = wav_get32(body + 4);
            fFileFormat.bits = container_bits;
            if (tag == WAVE_FORMAT_EXTENSIBLE && size >= 40) {
                tag = wav_get16(body + 24);
                fFileFormat.bits = wav_get16(body + 18);
            }
            if (tag != WAVE_FORMAT_PCM ||
                (container_bits != 8 && container_bits != 16 &&
                 container_bits != 32) ||
                fFileFormat.ContainerBytes() * 8 != container_bits ||
                fFileFormat.channels < 1 || fFileFormat.rate == 0) {
                fprintf(stderr, "%s: only 8, 16 and 32 bit integer PCM is "
                        "supported\n", fPath.c_str());
                return false;
            }
            have_fmt = true;
        } else if (memcmp(&file[pos], "data", 4) == 0 && have_fmt) {
            fData.assign(body, body + size);
            if (fFileFormat.ContainerBytes() == 1) {
                wav_flip_8bit(fData.data(), fData.size());
            }
            fFrames = fData.size() / fFileFormat.FrameBytes();
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    fprintf(stderr, "%s: no audio data\n", fPath.c_str());
    return false;
}


/* The file's samples, converted to the width and channel count the stream
   runs at; missing channels are silent. */
void WavReader::Read(uint8_t *buf, size_t frames)
{
    int in_bytes = fFileFormat.ContainerBytes();
    int out_bytes = fFormat.ContainerBytes();
    size_t in_frame = fFileFormat.FrameBytes();

    memset(buf, 0, frames * fFormat.FrameBytes());
    for (size_t i = 0; i < frames; i++) {
        if (fPos >= fFrames) {
            if (!fLoop || fFrames == 0) {
                return;
            }
            fPos = 0;
        }
        const uint8_t *src = &fData[fPos++ * in_frame];
        uint8_t *dst = buf + i * fFormat.FrameBytes();
        int channels = fFormat.channels < fFileFormat.channels
                           ? fFormat.channels : fFileFormat.channels;
        for (int c = 0; c < channels; c++) {
            uint32_t v = (uint32_t)audio_sample_get(src + c * in_bytes,
                                                    in_bytes);
            v >>= 32 - out_bytes * 8;
            for (int b = 0; b < out_bytes; b++) {
                dst[c * out_bytes + b] = (uint8_t)(v >> (8 * b));
            }
        }
    }
}


std::unique_ptr<HostAudio> wav_audio_open(EventLoop &loop,
                                          const AudioSettings &settings)
{
    if (settings.file == nullptr) {
        fprintf(stderr, "wav: expecting a 'file' property\n");
        return nullptr;
    }
    if (settings.direction == AUDIO_RENDER) {
        auto audio = std::make_unique<WavWriter>(loop, settings.file);
        if (!audio->Open()) {
            return nullptr;
        }
        return audio;
    }
    auto audio = std::make_unique<WavReader>(loop, settings.file,
                                             settings.loop);
    if (!audio->Open()) {
        return nullptr;
    }
    return audio;
}
