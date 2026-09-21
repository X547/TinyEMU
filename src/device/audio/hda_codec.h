/*
 * High Definition Audio codec
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

#include <stdint.h>

#include <memory>
#include <vector>

#include "device.h"
#include "host_audio.h"

/* No vendor's part, so that a driver treats it as the generic codec it is. */
#define HDA_CODEC_DEFAULT_VENDOR_ID 0x1af40010

/* The default device a pin's configuration names: what is plugged into it,
   and so which way sound goes. */
typedef enum {
    HDA_PIN_LINE_OUT  = 0x0,
    HDA_PIN_SPEAKER   = 0x1,
    HDA_PIN_HEADPHONE = 0x2,
    HDA_PIN_LINE_IN   = 0x8,
    HDA_PIN_MIC_IN    = 0xa,
} HDAPinDeviceEnum;


/* One port of an audio function group, as the configuration declares it. */
struct HDAPortConfig {
    HDAPinDeviceEnum kind = HDA_PIN_LINE_OUT;
    /* the location field of the pin configuration, -1 for the kind's own */
    int location = -1;
    int association = 1;
    int sequence = 0;
    int channels = 2;
    /* whether something is plugged into the jack */
    bool plugged = true;
    /* The rates the converter offers; empty for the host end's own. */
    std::vector<uint32_t> rates;
};

/* Names as the configuration spells them; false for one that is not. */
bool hda_pin_kind_from_name(const char *name, HDAPinDeviceEnum *out);
bool hda_pin_location_from_name(const char *name, int *out);


/* A codec on an HDA link; 'address' < 0 takes the first free one. */
Device *hda_codec_node_create(const char *name, int address,
                              uint32_t vendor_id, uint32_t subsystem_id,
                              uint32_t revision_id);

/* The audio function group of a codec, which its ports hang from. */
Device *hda_audio_group_node_create(const char *name);

/* A converter and the pin it drives, playing to 'audio'. */
Device *hda_output_node_create(const char *name, const HDAPortConfig &config,
                               std::unique_ptr<HostAudio> audio);
