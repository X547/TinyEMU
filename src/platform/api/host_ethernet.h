/*
 * Host network link
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

/* The device half of the link, installed on the host half once the device
   exists. */
class EthernetTarget {
public:
    virtual ~EthernetTarget() = default;

    virtual bool CanWritePacket() = 0;
    virtual void WritePacket(const uint8_t *buf, int len) = 0;
    virtual void SetCarrier(bool carrier_state) = 0;
};


/* The host half of the link: a tap interface, slirp, ... Frames from the
   host arrive at 'target' from the event loop. */
class HostEthernet {
public:
    uint8_t mac_addr[6] {}; /* mac address of the interface */
    EthernetTarget *target = nullptr; /* set by the device */

    virtual ~HostEthernet() = default;

    virtual void WritePacket(const uint8_t *buf, int len) = 0;
};
