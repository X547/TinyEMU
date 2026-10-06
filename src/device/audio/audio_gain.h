/*
 * Gain ramp shared by the audio devices
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
#pragma once

#include <stddef.h>
#include <stdint.h>

/* How long a change of gain takes, so that it is not heard as a click. */
#define AUDIO_GAIN_RAMP_MS 5


/* The gain of each channel of a stereo pair, gliding to what the guest set
   over a few milliseconds. A block of frames starts with Begin(), takes
   At() for each frame and ends with End(). */
class GainRamp {
private:
    float fGain[2] = {0.0f, 0.0f};
    float fTarget[2] = {0.0f, 0.0f};
    size_t fRamp = 1;

public:
    void Begin(float left, float right, uint32_t rate)
    {
        fTarget[0] = left;
        fTarget[1] = right;
        fRamp = rate * AUDIO_GAIN_RAMP_MS / 1000 + 1;
    }

    float At(int ch, size_t frame) const
    {
        return frame < fRamp ? fGain[ch] + (fTarget[ch] - fGain[ch]) *
                                               (float)(frame + 1) /
                                               (float)fRamp
                             : fTarget[ch];
    }

    void End(size_t frames)
    {
        for (int ch = 0; ch < 2; ch++) {
            fGain[ch] = frames >= fRamp
                        ? fTarget[ch]
                        : fGain[ch] + (fTarget[ch] - fGain[ch]) *
                                          (float)frames / (float)fRamp;
        }
    }
};
