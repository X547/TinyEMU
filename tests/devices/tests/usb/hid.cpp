/*
 * USB HID class tests
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

/* The USB transport of HID functions, by the Device Class Definition for
   Human Interface Devices, version 1.11: the HID and report descriptors
   (6.2), the class requests (7.2) and the input reports' interrupt
   endpoint. What goes into the reports is the functions' business and is
   tested with input, later. */

#define HID_GET_REPORT   0x01
#define HID_GET_IDLE     0x02
#define HID_GET_PROTOCOL 0x03
#define HID_SET_REPORT   0x09
#define HID_SET_IDLE     0x0a
#define HID_SET_PROTOCOL 0x0b
#define HID_INPUT   1
#define HID_OUTPUT  2
#define HID_FEATURE 3

#define HID_IN  (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define HID_OUT (USB_TYPE_CLASS | USB_RECIP_INTERFACE)

namespace {

struct Function {
    const uint8_t *iface;   /* in the configuration */
    const uint8_t *hid;     /* the HID descriptor after it */
    UsbEndpoint ep;
    bool boot_keyboard;
};

/* The interfaces of the first HID device. */
int open_hid(UsbDevice *d, Function *fns, int max, int index = 0)
{
    usb_require_device(USB_CLASS_HID, index, d);
    int n = 0;
    for (int i = 0; n < max; i++) {
        const uint8_t *id = usb_find_interface(d, i);
        if (id == nullptr)
            break;
        if (id[5] != USB_CLASS_HID)
            continue;
        Function &f = fns[n++];
        f.iface = id;
        f.hid = id + id[0];
        f.boot_keyboard = id[6] == 1 && id[7] == 1;
        EXPECT_TRUE(d->FindEndpoint(i, 0, &f.ep));
    }
    return n;
}

uint32_t req(UsbDevice &d, uint8_t type, uint8_t r, uint16_t value,
             uint16_t index, uint16_t len = 0, void *buf = nullptr,
             uint32_t *got = nullptr)
{
    uint32_t cc = d.Control(type, r, value, index, len, buf, got);
    if (cc == CC_STALL)
        d.Recover(1);
    return cc;
}

void snap_hex(const char *key, const uint8_t *p, uint32_t len)
{
    char hex[2 * 128 + 1];
    len = min(len, 128u);
    for (uint32_t i = 0; i < len; i++)
        snprintf(hex + 2 * i, 3, "%02x", p[i]);
    hex[2 * len] = 0;
    t_snap(key, "%s", hex);
}

}

TEST(hid, descriptors)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    ASSERT_GE(n, 1);
    uint8_t *buf = (uint8_t *)usb_alloc(256);
    for (int i = 0; i < n; i++) {
        Function &f = fns[i];
        uint8_t ifn = f.iface[2];
        /* the HID descriptor follows the interface's (7.1) */
        const uint8_t *h = f.hid;
        ASSERT_EQ(h[1], USB_DT_HID);
        EXPECT_EQ(h[0], 6 + 3 * h[5]);
        EXPECT_EQ(le16(h + 2), 0x0111);
        EXPECT_GE(h[5], 1);
        EXPECT_EQ(h[6], USB_DT_REPORT);
        uint16_t report_len = le16(h + 7);
        /* subclass 1 is boot, with protocol 1 keyboard or 2 mouse (4.2,
           4.3) */
        EXPECT_LE(f.iface[6], 1);
        if (f.iface[6] == 1)
            EXPECT_TRUE(f.iface[7] == 1 || f.iface[7] == 2);
        else
            EXPECT_EQ(f.iface[7], 0);
        /* an interrupt IN endpoint */
        EXPECT_TRUE(f.ep.address & USB_DIR_IN);
        EXPECT_EQ(f.ep.attributes & 3, 3);

        char k[32];
        snprintf(k, sizeof(k), "if%u.class", ifn);
        SNAP(k, "%u/%u/%u", f.iface[5], f.iface[6], f.iface[7]);
        snprintf(k, sizeof(k), "if%u.hid", ifn);
        snap_hex(k, h, h[0]);
        snprintf(k, sizeof(k), "if%u.endpoint", ifn);
        SNAP(k, "%#x %#x %u %u", f.ep.address, f.ep.attributes,
             f.ep.max_packet, f.ep.interval);

        /* the same HID descriptor by GET_DESCRIPTOR to the interface
           (7.1.1) */
        uint32_t got;
        ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                      USB_REQ_GET_DESCRIPTOR, USB_DT_HID << 8, ifn, 255, buf,
                      &got), (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, (uint32_t)h[0]);
        EXPECT_EQ(memcmp(buf, h, h[0]), 0);
        /* and the report descriptor, as long as it says */
        ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                      USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, ifn, 255,
                      buf, &got), (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, (uint32_t)report_len);
        snprintf(k, sizeof(k), "if%u.report", ifn);
        snap_hex(k, buf, got);
        /* starts with a Usage Page, ends with an End Collection (6.2.2) */
        EXPECT_EQ(buf[0] & 0xfc, 0x04);
        EXPECT_EQ(buf[got - 1], 0xc0);
        /* short */
        ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                      USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, ifn, 4,
                      buf, &got), (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, 4u);
        /* a physical descriptor is optional (6.2.3): snapshot */
        snprintf(k, sizeof(k), "if%u.physical", ifn);
        SNAP(k, "%u", req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                          USB_REQ_GET_DESCRIPTOR, 0x2300, ifn, 255, buf));
    }
    /* an interface it does not have */
    EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE, USB_REQ_GET_DESCRIPTOR,
                  USB_DT_REPORT << 8, 9, 255, buf), (uint32_t)CC_STALL);
}

TEST(hid, get_report)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    for (int i = 0; i < n; i++) {
        uint8_t ifn = fns[i].iface[2];
        uint32_t got;
        /* Get_Report of the input report: required (7.2.1) */
        memset(buf, 0xee, 64);
        ASSERT_EQ(req(d, HID_IN, HID_GET_REPORT, HID_INPUT << 8, ifn, 64, buf,
                      &got), (uint32_t)CC_SUCCESS);
        EXPECT_GE(got, 1u);
        EXPECT_LE(got, (uint32_t)fns[i].ep.max_packet);
        char k[32];
        snprintf(k, sizeof(k), "if%u.input", ifn);
        snap_hex(k, buf, got);
        /* shorter than the report */
        ASSERT_EQ(req(d, HID_IN, HID_GET_REPORT, HID_INPUT << 8, ifn, 2, buf,
                      &got), (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, 2u);
        /* the reports it has none of (snapshot) */
        snprintf(k, sizeof(k), "if%u.output", ifn);
        SNAP(k, "%u", req(d, HID_IN, HID_GET_REPORT, HID_OUTPUT << 8, ifn, 64,
                          buf));
        snprintf(k, sizeof(k), "if%u.feature", ifn);
        SNAP(k, "%u", req(d, HID_IN, HID_GET_REPORT, HID_FEATURE << 8, ifn,
                          64, buf));
    }
    EXPECT_EQ(req(d, HID_IN, HID_GET_REPORT, HID_INPUT << 8, 9, 64, buf),
              (uint32_t)CC_STALL);
    EXPECT_EQ(req(d, HID_IN, 0x42, 0, 0, 64, buf), (uint32_t)CC_STALL);
}

TEST(hid, set_report)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    for (int i = 0; i < n; i++) {
        uint8_t ifn = fns[i].iface[2];
        /* a boot keyboard takes its LED output report (B.1) */
        buf[0] = 0x05;
        uint32_t cc = req(d, HID_OUT, HID_SET_REPORT, HID_OUTPUT << 8, ifn, 1,
                          buf);
        if (fns[i].boot_keyboard) {
            EXPECT_EQ(cc, (uint32_t)CC_SUCCESS);
            /* and reads back by Get_Report (7.2.1) */
            uint32_t got;
            buf[0] = 0;
            ASSERT_EQ(req(d, HID_IN, HID_GET_REPORT, HID_OUTPUT << 8, ifn, 8,
                          buf, &got), (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 1u);
            EXPECT_EQ(buf[0], 0x05);
        }
        char k[32];
        snprintf(k, sizeof(k), "if%u.output", ifn);
        SNAP(k, "%u", cc);
        snprintf(k, sizeof(k), "if%u.feature", ifn);
        SNAP(k, "%u", req(d, HID_OUT, HID_SET_REPORT, HID_FEATURE << 8, ifn,
                          1, buf));
        snprintf(k, sizeof(k), "if%u.input", ifn);
        SNAP(k, "%u", req(d, HID_OUT, HID_SET_REPORT, HID_INPUT << 8, ifn, 8,
                          buf));
    }
}

TEST(hid, idle)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    for (int i = 0; i < n; i++) {
        uint8_t ifn = fns[i].iface[2];
        uint32_t got;
        /* the default (7.2.4: 500 ms recommended for keyboards, 0 for
           mice) */
        ASSERT_EQ(req(d, HID_IN, HID_GET_IDLE, 0, ifn, 1, buf, &got),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, 1u);
        char k[32];
        snprintf(k, sizeof(k), "if%u.default", ifn);
        SNAP(k, "%u", buf[0]);
        /* Set_Idle: the duration in the upper byte, 4 ms units (7.2.4) */
        ASSERT_EQ(req(d, HID_OUT, HID_SET_IDLE, 125 << 8, ifn),
                  (uint32_t)CC_SUCCESS);
        ASSERT_EQ(req(d, HID_IN, HID_GET_IDLE, 0, ifn, 1, buf),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(buf[0], 125);
        ASSERT_EQ(req(d, HID_OUT, HID_SET_IDLE, 0, ifn), (uint32_t)CC_SUCCESS);
        ASSERT_EQ(req(d, HID_IN, HID_GET_IDLE, 0, ifn, 1, buf),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(buf[0], 0);
    }
}

/* With a non-zero idle rate the device reports even without a change,
   once the period is over (7.2.4). */
TEST(hid, idle_rate_reports)
{
    UsbDevice d;
    Function fns[4];
    open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    Function &f = fns[0];
    int dci = usb_dci(f.ep.address);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    ASSERT_EQ(req(d, HID_OUT, HID_SET_IDLE, 4 << 8, f.iface[2]),
              (uint32_t)CC_SUCCESS);
    uint64_t t0 = t_now_us();
    uint32_t got;
    EXPECT_EQ(d.Transfer(dci, buf, f.ep.max_packet, &got, 1000000),
              (uint32_t)CC_SUCCESS);
    uint64_t t1 = t_now_us();
    EXPECT_EQ(d.Transfer(dci, buf, f.ep.max_packet, &got, 1000000),
              (uint32_t)CC_SUCCESS);
    uint64_t t2 = t_now_us();
    /* 16 ms each; not before (some of) the period */
    EXPECT_GE(t2 - t1, 8000u);
    (void)t0;
    ASSERT_EQ(req(d, HID_OUT, HID_SET_IDLE, 0, f.iface[2]),
              (uint32_t)CC_SUCCESS);
}

TEST(hid, protocol)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    for (int i = 0; i < n; i++) {
        uint8_t ifn = fns[i].iface[2];
        bool boot = fns[i].iface[6] == 1;
        uint32_t got;
        /* the report protocol after reset (7.2.6) */
        uint32_t cc = req(d, HID_IN, HID_GET_PROTOCOL, 0, ifn, 1, buf, &got);
        if (boot) {
            ASSERT_EQ(cc, (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 1u);
            EXPECT_EQ(buf[0], 1);
            ASSERT_EQ(req(d, HID_OUT, HID_SET_PROTOCOL, 0, ifn),
                      (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, HID_IN, HID_GET_PROTOCOL, 0, ifn, 1, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(buf[0], 0);
            /* the boot keyboard report: 8 bytes (B.1) */
            if (fns[i].boot_keyboard) {
                ASSERT_EQ(req(d, HID_IN, HID_GET_REPORT, HID_INPUT << 8, ifn,
                              64, buf, &got), (uint32_t)CC_SUCCESS);
                EXPECT_EQ(got, 8u);
            }
            ASSERT_EQ(req(d, HID_OUT, HID_SET_PROTOCOL, 1, ifn),
                      (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, HID_IN, HID_GET_PROTOCOL, 0, ifn, 1, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(buf[0], 1);
        } else {
            /* only boot devices must have the protocol requests (7.2.5,
               7.2.6): snapshot */
            char k[32];
            snprintf(k, sizeof(k), "if%u.get", ifn);
            SNAP(k, "%u/%u", cc, cc == CC_SUCCESS ? buf[0] : 0);
            snprintf(k, sizeof(k), "if%u.set_boot", ifn);
            SNAP(k, "%u", req(d, HID_OUT, HID_SET_PROTOCOL, 0, ifn));
        }
    }
}

TEST(hid, protocol_reset)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    if (d.route != 0)
        t_skip("behind a hub");
    int boot = -1;
    for (int i = 0; i < n && boot < 0; i++) {
        if (fns[i].iface[6] == 1)
            boot = i;
    }
    if (boot < 0)
        t_skip("no boot interface");
    uint8_t ifn = fns[boot].iface[2];
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    ASSERT_EQ(req(d, HID_OUT, HID_SET_PROTOCOL, 0, ifn),
              (uint32_t)CC_SUCCESS);
    ASSERT_EQ(req(d, HID_OUT, HID_SET_IDLE, 10 << 8, ifn),
              (uint32_t)CC_SUCCESS);
    /* a bus reset puts the report protocol back (7.2.6) */
    UsbDevice again;
    ASSERT_TRUE(d.hc->PortReset(d.root_port));
    again = d;
    ASSERT_EQ(again.Address(), (uint32_t)CC_SUCCESS);
    ASSERT_TRUE(again.ReadDescriptors());
    ASSERT_EQ(again.Configure(), (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    ASSERT_EQ(req(again, HID_IN, HID_GET_PROTOCOL, 0, ifn, 1, buf),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(buf[0], 1);
    ASSERT_EQ(req(again, HID_IN, HID_GET_IDLE, 0, ifn, 1, buf),
              (uint32_t)CC_SUCCESS);
    SNAP("idle_after_reset", "%u", buf[0]);
}

/* With nothing to report the interrupt IN endpoint NAKs: the TD waits,
   and the control endpoint goes on meanwhile. */
TEST(hid, interrupt_in_waits)
{
    UsbDevice d;
    Function fns[4];
    int n = open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(64 * 4);
    for (int i = 0; i < n; i++)
        d.Queue(usb_dci(fns[i].ep.address), buf + 64 * i,
                fns[i].ep.max_packet);
    for (int i = 0; i < n; i++)
        EXPECT_EQ(d.WaitTransfer(usb_dci(fns[i].ep.address), nullptr, 20000),
                  (uint32_t)CC_TIMEOUT);
    uint8_t *rep = (uint8_t *)usb_alloc(64);
    EXPECT_EQ(req(d, HID_IN, HID_GET_REPORT, HID_INPUT << 8,
                  fns[0].iface[2], 64, rep), (uint32_t)CC_SUCCESS);
    /* a second TD queued behind the first waits as well */
    int dci = usb_dci(fns[0].ep.address);
    d.Queue(dci, buf + 192, fns[0].ep.max_packet);
    EXPECT_EQ(d.WaitTransfer(dci, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    /* and both go when the endpoint is stopped (4.6.9) */
    Trb ev;
    ASSERT_EQ(d.StopEndpoint(dci), (uint32_t)CC_SUCCESS);
    ASSERT_NE(d.WaitTransfer(dci, nullptr, 20000, &ev), (uint32_t)CC_TIMEOUT);
    EXPECT_EQ(d.WaitTransfer(dci, nullptr, 20000), (uint32_t)CC_TIMEOUT);
}

/* Endpoint halt by the standard feature (9.4.5): the interrupt pipe
   stalls until cleared. */
TEST(hid, halted_endpoint)
{
    UsbDevice d;
    Function fns[4];
    open_hid(&d, fns, 4);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    UsbEndpoint &ep = fns[0].ep;
    int dci = usb_dci(ep.address);
    ASSERT_EQ(req(d, USB_RECIP_ENDPOINT, USB_REQ_SET_FEATURE, 0, ep.address),
              (uint32_t)CC_SUCCESS);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    EXPECT_EQ(d.Transfer(dci, buf, ep.max_packet, nullptr, 200000),
              (uint32_t)CC_STALL);
    EXPECT_EQ(d.EpState(dci), (uint32_t)EP_HALTED);
    ASSERT_TRUE(d.Recover(dci));
    EXPECT_EQ(d.Transfer(dci, buf, ep.max_packet, nullptr, 20000),
              (uint32_t)CC_TIMEOUT);
}
