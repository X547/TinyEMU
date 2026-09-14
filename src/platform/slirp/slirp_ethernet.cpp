/*
 * Host network link: user mode network stack (slirp)
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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

/* libslirp.h expects fd_set to be declared */
#include "wait_set.h"

#include "slirp/libslirp.h"

#include "host_ethernet.h"
#include "platform_backends.h"


class SlirpEthernet final: public HostEthernet, public PollSource {
public:
    EventLoop &fLoop;
    Slirp *state = nullptr;

    SlirpEthernet(EventLoop &loop): fLoop(loop) {fLoop.Add(this);}
    ~SlirpEthernet() override;

    /* HostEthernet */
    void WritePacket(const uint8_t *buf, int len) override
    {
        slirp_input(state, buf, len);
    }

    /* PollSource */
    void Prepare(WaitSet &ws) override
    {
        slirp_select_fill(state, &ws.fd_max, &ws.rfds, &ws.wfds, &ws.efds);
    }

    void Dispatch(WaitSet &ws) override
    {
        slirp_select_poll(state, &ws.rfds, &ws.wfds, &ws.efds, ws.ready <= 0);
    }
};

/* slirp keeps global state, so there is only ever one */
static Slirp *slirp_state;

SlirpEthernet::~SlirpEthernet()
{
    fLoop.Remove(this);
    if (state != nullptr) {
        slirp_cleanup(state);
        if (slirp_state == state)
            slirp_state = nullptr;
    }
}

/* Provided to slirp, which is compiled as C. */
extern "C" int slirp_can_output(void *opaque)
{
    HostEthernet *net = static_cast<HostEthernet *>(opaque);
    return net->target != nullptr && net->target->CanWritePacket();
}

extern "C" void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    HostEthernet *net = static_cast<HostEthernet *>(opaque);
    if (net->target != nullptr)
        net->target->WritePacket(pkt, pkt_len);
}


std::unique_ptr<HostEthernet> slirp_ethernet_open(EventLoop &loop)
{
    struct in_addr net_addr  = { .s_addr = htonl(0x0a000200) }; /* 10.0.2.0 */
    struct in_addr mask = { .s_addr = htonl(0xffffff00) }; /* 255.255.255.0 */
    struct in_addr host = { .s_addr = htonl(0x0a000202) }; /* 10.0.2.2 */
    struct in_addr dhcp = { .s_addr = htonl(0x0a00020f) }; /* 10.0.2.15 */
    struct in_addr dns  = { .s_addr = htonl(0x0a000203) }; /* 10.0.2.3 */
    const char *bootfile = NULL;
    const char *vhostname = NULL;
    int restricted = 0;

    if (slirp_state) {
        fprintf(stderr, "Only a single slirp instance is allowed\n");
        return nullptr;
    }
    auto net = std::make_unique<SlirpEthernet>(loop);

    slirp_state = slirp_init(restricted, net_addr, mask, host, vhostname,
                             "", bootfile, dhcp, dns, net.get());
    net->state = slirp_state;

    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;

    return net;
}
