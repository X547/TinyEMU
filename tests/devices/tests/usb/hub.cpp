/*
 * USB hub tests
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
#include "format.h"
#include "test.h"
#include "usb.h"

/* A USB 2.0 hub as chapter 11 of the Universal Serial Bus Specification,
   revision 2.0, has it: its descriptor (11.23.2.1), the class requests
   (11.24.2) and the port state they report, and the status change
   endpoint (11.12.4). The devices behind it are reached by route string,
   so the hub itself never carries their transfers. */

#define HUB_REQ_OUT  (USB_TYPE_CLASS | USB_RECIP_OTHER)
#define HUB_REQ_IN   (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER)
#define HUB_HREQ_OUT USB_TYPE_CLASS
#define HUB_HREQ_IN  (USB_DIR_IN | USB_TYPE_CLASS)

/* port features (table 11-17) */
#define PORT_CONNECTION     0
#define PORT_ENABLE         1
#define PORT_SUSPEND        2
#define PORT_OVER_CURRENT   3
#define PORT_RESET          4
#define PORT_POWER          8
#define PORT_LOW_SPEED      9
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_SUSPEND      18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET        20
#define PORT_TEST           21
#define PORT_INDICATOR      22

/* wPortStatus and wPortChange (11.24.2.7) */
#define PS_CONNECTION 0x0001
#define PS_ENABLE     0x0002
#define PS_SUSPEND    0x0004
#define PS_RESET      0x0010
#define PS_POWER      0x0100
#define PS_LOW_SPEED  0x0200
#define PS_HIGH_SPEED 0x0400
#define PC_CONNECTION 0x0001
#define PC_ENABLE     0x0002
#define PC_SUSPEND    0x0004
#define PC_RESET      0x0010

namespace {

struct Hub {
    UsbDevice d;
    int ports;
    int dci;            /* the status change endpoint */
    uint16_t mps;
    uint8_t *buf;
};

/* The first hub, addressed, configured, its ports as configuration left
   them. */
void open_hub(Hub *h)
{
    usb_require_device(USB_CLASS_HUB, 0, &h->d);
    h->buf = (uint8_t *)usb_alloc(64);
    ASSERT_EQ(h->d.Control(HUB_HREQ_IN, USB_REQ_GET_DESCRIPTOR,
                           USB_DT_HUB << 8, 0, 64, h->buf),
              (uint32_t)CC_SUCCESS);
    h->ports = h->buf[2];
    h->d.hub_ports = h->ports;
    ASSERT_EQ(h->d.Configure(), (uint32_t)CC_SUCCESS);
    UsbEndpoint ep;
    ASSERT_TRUE(h->d.FindEndpoint(0, 0, &ep));
    h->dci = usb_dci(ep.address);
    h->mps = ep.max_packet;
}

uint32_t port_req(Hub *h, uint8_t req, uint16_t feature, uint16_t port)
{
    uint32_t cc = h->d.Control(HUB_REQ_OUT, req, feature, port, 0, nullptr);
    if (cc == CC_STALL)
        h->d.Recover(1);
    return cc;
}

/* wPortStatus | wPortChange << 16, or ~0 when it failed */
uint32_t port_status(Hub *h, int port)
{
    uint32_t got;
    uint32_t cc = h->d.Control(HUB_REQ_IN, USB_REQ_GET_STATUS, 0, port, 4,
                               h->buf, &got);
    if (cc == CC_STALL)
        h->d.Recover(1);
    if (cc != CC_SUCCESS || got != 4)
        return ~0u;
    return le32(h->buf);
}

void power_all(Hub *h)
{
    for (int p = 1; p <= h->ports; p++)
        ASSERT_EQ(port_req(h, USB_REQ_SET_FEATURE, PORT_POWER, p),
                  (uint32_t)CC_SUCCESS);
}

void clear_changes(Hub *h, int p)
{
    static const uint16_t kChanges[] = {C_PORT_CONNECTION, C_PORT_ENABLE,
                                        C_PORT_SUSPEND, C_PORT_OVER_CURRENT,
                                        C_PORT_RESET};
    for (uint16_t f : kChanges)
        EXPECT_EQ(port_req(h, USB_REQ_CLEAR_FEATURE, f, p),
                  (uint32_t)CC_SUCCESS);
}

/* The ports with something attached, once powered. */
uint32_t connected_ports(Hub *h)
{
    uint32_t mask = 0;
    for (int p = 1; p <= h->ports; p++) {
        if (port_status(h, p) & PS_CONNECTION)
            mask |= 1u << p;
    }
    return mask;
}

bool reset_port(Hub *h, int p, uint32_t *status)
{
    if (port_req(h, USB_REQ_SET_FEATURE, PORT_RESET, p) != CC_SUCCESS)
        return false;
    return t_wait_until([&] {
        *status = port_status(h, p);
        return *status != ~0u && !(*status & PS_RESET) &&
               (*status >> 16 & PC_RESET);
    }, 500000);
}

}

TEST(hub, descriptor)
{
    Hub h;
    open_hub(&h);
    uint8_t *b = (uint8_t *)usb_alloc(64);
    uint32_t got;
    ASSERT_EQ(h.d.Control(HUB_HREQ_IN, USB_REQ_GET_DESCRIPTOR,
                          USB_DT_HUB << 8, 0, 64, b, &got),
              (uint32_t)CC_SUCCESS);
    /* 7 bytes, then DeviceRemovable and PortPwrCtrlMask, a bit per port
       and one for the hub, in whole bytes (11.23.2.1) */
    int var = (h.ports + 1 + 7) / 8;
    EXPECT_EQ(b[0], 7 + 2 * var);
    EXPECT_EQ(got, (uint32_t)b[0]);
    EXPECT_EQ(b[1], USB_DT_HUB);
    EXPECT_GE(h.ports, 1);
    EXPECT_LE(h.ports, 255);
    uint16_t chars = le16(b + 3);
    /* reserved bits 15:8 (no port indicators named here: bit 7 snapped) */
    EXPECT_EQ(chars & 0xff00, 0);
    /* a single TT hub: TT think time any; power switching and
       over-current protection are 0, 1 or 2 */
    EXPECT_LE(chars & 3, 2);
    EXPECT_LE((chars >> 3) & 3, 2);
    /* the hub's own bit 0 of DeviceRemovable is reserved, zero */
    EXPECT_EQ(b[7] & 1, 0);
    /* PortPwrCtrlMask is all ones for compatibility */
    for (int i = 0; i < var; i++)
        EXPECT_EQ(b[7 + var + i], 0xff);
    char hex[2 * 64 + 1];
    for (uint32_t i = 0; i < got; i++)
        snprintf(hex + 2 * i, 3, "%02x", b[i]);
    SNAP("descriptor", "%s", hex);
    /* short */
    ASSERT_EQ(h.d.Control(HUB_HREQ_IN, USB_REQ_GET_DESCRIPTOR,
                          USB_DT_HUB << 8, 0, 2, b, &got),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(got, 2u);
    /* a hub is class 9, with the protocol of its TTs (11.23.1) */
    EXPECT_EQ(h.d.desc[4], USB_CLASS_HUB);
    SNAP("protocol", "%u", h.d.desc[6]);
    const uint8_t *id = usb_find_interface(&h.d, 0);
    EXPECT_EQ(id[5], USB_CLASS_HUB);
    EXPECT_EQ(id[4], 1);
    /* the status change endpoint: interrupt IN, room for the bitmap */
    UsbEndpoint ep;
    ASSERT_TRUE(h.d.FindEndpoint(0, 0, &ep));
    EXPECT_TRUE(ep.address & USB_DIR_IN);
    EXPECT_EQ(ep.attributes & 3, 3);
    EXPECT_GE(ep.max_packet, var);
}

TEST(hub, hub_status)
{
    Hub h;
    open_hub(&h);
    uint32_t got;
    /* wHubStatus and wHubChange: power good, no over-current (11.24.2.6) */
    ASSERT_EQ(h.d.Control(HUB_HREQ_IN, USB_REQ_GET_STATUS, 0, 0, 4, h.buf,
                          &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(le32(h.buf), 0u);
    /* the hub change bits clear (11.24.2.1) */
    EXPECT_EQ(h.d.Control(HUB_HREQ_OUT, USB_REQ_CLEAR_FEATURE, 0, 0, 0,
                          nullptr), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(h.d.Control(HUB_HREQ_OUT, USB_REQ_CLEAR_FEATURE, 1, 0, 0,
                          nullptr), (uint32_t)CC_SUCCESS);
    /* there is no hub feature to set (11.24.2.12): snapshot */
    uint32_t cc = h.d.Control(HUB_HREQ_OUT, USB_REQ_SET_FEATURE, 0, 0, 0,
                              nullptr);
    SNAP("set_hub_feature", "%u", cc);
    if (cc == CC_STALL)
        h.d.Recover(1);
    /* the transaction translator requests a high speed hub takes, with
       wIndex 1 for its single TT (11.24.2.3, 11.24.2.9, 11.24.2.11) */
    if (h.d.speed == SPEED_HIGH) {
        static const uint8_t kTT[] = {8, 9, 11};
        for (uint8_t r : kTT)
            EXPECT_EQ(h.d.Control(HUB_REQ_OUT, r, 0, 1, 0, nullptr),
                      (uint32_t)CC_SUCCESS);
    }
    /* GET_TT_STATE is optional (11.24.2.6): snapshot */
    cc = h.d.Control(HUB_REQ_IN, 10, 0, 1, 4, h.buf);
    SNAP("get_tt_state", "%u", cc);
    if (cc == CC_STALL)
        h.d.Recover(1);
}

/* Ports switched one by one (11.11): powered off after configuration,
   then a connection seen once powered. */
TEST(hub, port_power)
{
    Hub h;
    open_hub(&h);
    for (int p = 1; p <= h.ports; p++) {
        uint32_t s = port_status(&h, p);
        ASSERT_NE(s, ~0u);
        /* Powered-off: no power, no connection (11.5.1.1) */
        EXPECT_EQ(s & 0xffff, 0u);
    }
    power_all(&h);
    uint32_t mask = 0;
    for (int p = 1; p <= h.ports; p++) {
        uint32_t s = port_status(&h, p);
        EXPECT_TRUE(s & PS_POWER);
        EXPECT_EQ(s & (PS_ENABLE | PS_SUSPEND | PS_RESET), 0u);
        /* a connection after power on is a change (11.24.2.7.2.1) */
        if (s & PS_CONNECTION) {
            mask |= 1u << p;
            EXPECT_TRUE(s >> 16 & PC_CONNECTION);
        } else {
            EXPECT_EQ(s >> 16, 0u);
        }
    }
    SNAP("connected", "%#x", mask);
    ASSERT_NE(mask, 0u);
    /* power off again: nothing is reported (11.5.1.1) */
    for (int p = 1; p <= h.ports; p++) {
        clear_changes(&h, p);
        EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, PORT_POWER, p),
                  (uint32_t)CC_SUCCESS);
        uint32_t s = port_status(&h, p);
        EXPECT_EQ(s & (PS_POWER | PS_CONNECTION | PS_ENABLE), 0u);
    }
}

TEST(hub, port_reset)
{
    Hub h;
    open_hub(&h);
    power_all(&h);
    uint32_t mask = connected_ports(&h);
    if (mask == 0)
        t_skip("nothing behind the hub");
    for (int p = 1; p <= h.ports; p++) {
        clear_changes(&h, p);
        uint32_t s;
        if (!(mask & (1u << p))) {
            /* nothing there (snapshot) */
            port_req(&h, USB_REQ_SET_FEATURE, PORT_RESET, p);
            char k[16];
            snprintf(k, sizeof(k), "empty%d", p);
            SNAP(k, "%#x", port_status(&h, p));
            clear_changes(&h, p);
            continue;
        }
        /* reset, then enabled with C_PORT_RESET and the speed (11.24.2.13,
           11.24.2.7.1) */
        ASSERT_TRUE(reset_port(&h, p, &s));
        EXPECT_TRUE(s & PS_ENABLE);
        EXPECT_TRUE(s & PS_CONNECTION);
        EXPECT_FALSE((s & PS_LOW_SPEED) && (s & PS_HIGH_SPEED));
        char k[16];
        snprintf(k, sizeof(k), "port%d", p);
        SNAP(k, "%#x", s);
        EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, C_PORT_RESET, p),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(port_status(&h, p) >> 16 & PC_RESET, 0u);

        /* disabled by software: no C_PORT_ENABLE (11.24.2.7.2.2) */
        EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, PORT_ENABLE, p),
                  (uint32_t)CC_SUCCESS);
        s = port_status(&h, p);
        EXPECT_EQ(s & PS_ENABLE, 0u);
        EXPECT_EQ(s >> 16 & PC_ENABLE, 0u);
        ASSERT_TRUE(reset_port(&h, p, &s));
        EXPECT_TRUE(s & PS_ENABLE);
        clear_changes(&h, p);
    }
}

TEST(hub, port_suspend)
{
    Hub h;
    open_hub(&h);
    power_all(&h);
    uint32_t mask = connected_ports(&h);
    if (mask == 0)
        t_skip("nothing behind the hub");
    for (int p = 1; p <= h.ports; p++) {
        if (!(mask & (1u << p)))
            continue;
        uint32_t s;
        ASSERT_TRUE(reset_port(&h, p, &s));
        clear_changes(&h, p);
        /* suspend, then resume: C_PORT_SUSPEND once the resume is done
           (11.24.2.7.1.3, 11.24.2.7.2.3) */
        EXPECT_EQ(port_req(&h, USB_REQ_SET_FEATURE, PORT_SUSPEND, p),
                  (uint32_t)CC_SUCCESS);
        s = port_status(&h, p);
        EXPECT_TRUE(s & PS_SUSPEND);
        EXPECT_TRUE(s & PS_ENABLE);
        EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, PORT_SUSPEND, p),
                  (uint32_t)CC_SUCCESS);
        EXPECT_TRUE(t_wait_until([&] {
            uint32_t v = port_status(&h, p);
            return v != ~0u && !(v & PS_SUSPEND) && (v >> 16 & PC_SUSPEND);
        }, 200000));
        EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, C_PORT_SUSPEND, p),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(port_status(&h, p) >> 16, 0u);
        /* the indicator, without indicators (11.24.2.7.1.10): snapshot */
        uint32_t cc = port_req(&h, USB_REQ_SET_FEATURE,
                               PORT_INDICATOR, p | 1 << 8);
        char k[24];
        snprintf(k, sizeof(k), "indicator%d", p);
        SNAP(k, "%u", cc);
        break;
    }
}

TEST(hub, bad_requests)
{
    Hub h;
    open_hub(&h);
    /* port 0 and past bNbrPorts (11.24.2) */
    EXPECT_EQ(port_status(&h, 0), ~0u);
    EXPECT_EQ(port_status(&h, h.ports + 1), ~0u);
    EXPECT_EQ(port_req(&h, USB_REQ_SET_FEATURE, PORT_POWER, 0),
              (uint32_t)CC_STALL);
    EXPECT_EQ(port_req(&h, USB_REQ_SET_FEATURE, PORT_POWER, h.ports + 1),
              (uint32_t)CC_STALL);
    EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, C_PORT_CONNECTION,
                       h.ports + 1), (uint32_t)CC_STALL);
    /* features that cannot be set or cleared (table 11-17) */
    EXPECT_EQ(port_req(&h, USB_REQ_SET_FEATURE, PORT_ENABLE, 1),
              (uint32_t)CC_STALL);
    EXPECT_EQ(port_req(&h, USB_REQ_SET_FEATURE, PORT_CONNECTION, 1),
              (uint32_t)CC_STALL);
    EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, PORT_CONNECTION, 1),
              (uint32_t)CC_STALL);
    EXPECT_EQ(port_req(&h, USB_REQ_CLEAR_FEATURE, PORT_RESET, 1),
              (uint32_t)CC_STALL);
    EXPECT_EQ(port_req(&h, USB_REQ_SET_FEATURE, 0x55, 1),
              (uint32_t)CC_STALL);
    /* a hub descriptor index other than 0, and the other descriptor
       types by the class request */
    uint8_t *b = (uint8_t *)usb_alloc(64);
    uint32_t cc = h.d.Control(HUB_HREQ_IN, USB_REQ_GET_DESCRIPTOR,
                              USB_DT_DEVICE << 8, 0, 18, b);
    SNAP("class_device_desc", "%u", cc);
    if (cc == CC_STALL)
        h.d.Recover(1);
    /* the port test feature (snapshot) */
    cc = port_req(&h, USB_REQ_SET_FEATURE, PORT_TEST, 1 | 5 << 8);
    SNAP("port_test", "%u", cc);
    /* an unknown class request */
    cc = h.d.Control(HUB_HREQ_IN, 0x42, 0, 0, 4, b);
    EXPECT_EQ(cc, (uint32_t)CC_STALL);
    if (cc == CC_STALL)
        h.d.Recover(1);
}

/* The status change endpoint (11.12.4): a bit per port with a change, bit
   0 for the hub, and a NAK while there is none. */
TEST(hub, status_change_endpoint)
{
    Hub h;
    open_hub(&h);
    power_all(&h);
    uint32_t mask = connected_ports(&h);
    if (mask == 0)
        t_skip("nothing behind the hub");
    uint8_t *bitmap = (uint8_t *)usb_alloc(64);
    uint32_t got;
    ASSERT_EQ(h.d.Transfer(h.dci, bitmap, h.mps, &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(got, (uint32_t)(h.ports + 1 + 7) / 8);
    uint32_t bits = bitmap[0] | (got > 1 ? bitmap[1] << 8 : 0);
    EXPECT_EQ(bits, mask);

    for (int p = 1; p <= h.ports; p++)
        clear_changes(&h, p);
    /* nothing changed: the transfer waits */
    memset(bitmap, 0xee, 8);
    uint64_t trb = h.d.Queue(h.dci, bitmap, h.mps);
    EXPECT_EQ(h.d.WaitTransfer(h.dci, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    /* a reset is a change, and answers it */
    int p = 1;
    while (!(mask & (1u << p)))
        p++;
    ASSERT_EQ(port_req(&h, USB_REQ_SET_FEATURE, PORT_RESET, p),
              (uint32_t)CC_SUCCESS);
    Trb ev;
    uint32_t residual;
    ASSERT_EQ(h.d.WaitTransfer(h.dci, &residual, 500000, &ev),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(ev.Param(), trb);
    EXPECT_EQ(bitmap[0], 1u << p);
    clear_changes(&h, p);
}

/* What is behind the hub is addressed by route string, and its transfers
   reach it; with a full speed device behind a high speed hub the slot names
   the hub's transaction translator. */
TEST(hub, downstream_devices)
{
    UsbDevice *devs = (UsbDevice *)usb_alloc(16 * sizeof(UsbDevice));
    int n = usb_find_all(devs, 16);
    int behind = 0;
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = devs[i];
        if (d.route == 0)
            continue;
        behind++;
        EXPECT_EQ(le32(d.OutSlot()) & 0xfffff, d.route);
        EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_ADDRESSED);
        EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(memcmp(buf, d.desc, 18), 0);
        char k[24];
        snprintf(k, sizeof(k), "route%#x", d.route);
        SNAP(k, "speed %u tt %u/%u", d.speed, d.tt_slot != 0, d.tt_port);
    }
    if (behind == 0)
        t_skip("nothing behind a hub");
    /* a route through a port with nothing on it */
    for (int i = 0; i < n; i++) {
        UsbDevice &hub = devs[i];
        if (hub.hub_ports == 0)
            continue;
        UsbDevice ghost {};
        ghost.hc = hub.hc;
        ghost.root_port = hub.root_port;
        ghost.speed = SPEED_FULL;
        for (int p = 1; p <= hub.hub_ports; p++) {
            bool used = false;
            for (int k = 0; k < n; k++)
                used |= devs[k].route == (hub.route | (uint32_t)p);
            if (!used) {
                ghost.route = hub.route | p;
                break;
            }
        }
        if (ghost.route == 0)
            break;
        EXPECT_EQ(ghost.Address(), (uint32_t)CC_TRANSACTION);
        break;
    }
}
