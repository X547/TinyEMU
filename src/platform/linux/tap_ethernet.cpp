/*
 * Host network link: tap interface
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
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include "cutils.h"
#include "host_ethernet.h"
#include "platform_backends.h"
#include "wait_set.h"


class TapEthernet final: public HostEthernet, public PollSource {
private:
    EventLoop &fLoop;
    int fFd;
    bool fWatching = false;

public:
    TapEthernet(EventLoop &loop, int fd): fLoop(loop), fFd(fd)
    {
        fLoop.Add(this);
    }

    ~TapEthernet() override
    {
        fLoop.Remove(this);
        close(fFd);
    }

    /* HostEthernet */
    void WritePacket(const uint8_t *buf, int len) override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};


void TapEthernet::WritePacket(const uint8_t *buf, int len)
{
    if (write(fFd, buf, len) < 0) {
        /* dropped, as a wire would */
    }
}


void TapEthernet::Prepare(WaitSet &ws)
{
    fWatching = target != nullptr && target->CanWritePacket();
    if (fWatching)
        ws.WatchRead(fFd);
}


void TapEthernet::Dispatch(WaitSet &ws)
{
    uint8_t buf[2048];
    int ret;

    if (!fWatching || !ws.IsReadable(fFd))
        return;
    ret = read(fFd, buf, sizeof(buf));
    if (ret > 0)
        target->WritePacket(buf, ret);
}


/* configure with:
# bridge configuration (connect tap0 to bridge interface br0)
   ip link add br0 type bridge
   ip tuntap add dev tap0 mode tap [user x] [group x]
   ip link set tap0 master br0
   ip link set dev br0 up
   ip link set dev tap0 up

# NAT configuration (eth1 is the interface connected to internet)
   ifconfig br0 192.168.3.1
   echo 1 > /proc/sys/net/ipv4/ip_forward
   iptables -D FORWARD 1
   iptables -t nat -A POSTROUTING -o eth1 -j MASQUERADE

   In the VM:
   ifconfig eth0 192.168.3.2
   route add -net 0.0.0.0 netmask 0.0.0.0 gw 192.168.3.1
*/
std::unique_ptr<HostEthernet> tap_ethernet_open(EventLoop &loop,
                                                const char *ifname)
{
    struct ifreq ifr;
    int fd, ret;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: could not open /dev/net/tun\n");
        return nullptr;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    pstrcpy(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        fprintf(stderr, "Error: could not configure /dev/net/tun\n");
        close(fd);
        return nullptr;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    auto s = std::make_unique<TapEthernet>(loop, fd);
    s->mac_addr[0] = 0x02;
    s->mac_addr[1] = 0x00;
    s->mac_addr[2] = 0x00;
    s->mac_addr[3] = 0x00;
    s->mac_addr[4] = 0x00;
    s->mac_addr[5] = 0x01;
    return s;
}
