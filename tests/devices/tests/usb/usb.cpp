/*
 * USB device framework tests
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

/* What every USB device does, whatever its class: the standard requests
   and descriptors of the Universal Serial Bus Specification, revision 2.0,
   chapter 9. Each test runs on every device of every host, the hubs and
   what is behind them included. */

#define MAX_DEVICES 16

#define FEATURE_ENDPOINT_HALT 0
#define FEATURE_REMOTE_WAKEUP 1

namespace {

UsbDevice *sDevs;

int all_devices()
{
    sDevs = (UsbDevice *)usb_alloc(MAX_DEVICES * sizeof(UsbDevice));
    int n = usb_find_all(sDevs, MAX_DEVICES);
    if (n == 0)
        t_skip("no USB device");
    return n;
}

/* "xhci.p3", "xhci-pci.p1.2": the host and the path to the device. */
const char *dev_name(const UsbDevice &d, char *buf, size_t size)
{
    int len = snprintf(buf, size, "%s.p%u", d.hc->Name(), d.root_port);
    for (uint32_t r = d.route; r & 0xf; r >>= 4)
        len += snprintf(buf + len, size - len, ".%u", r & 0xf);
    return buf;
}

void snap_dev(const UsbDevice &d, const char *key, const char *fmt,
              uint32_t v)
{
    char name[32], k[64];
    snprintf(k, sizeof(k), "%s.%s", dev_name(d, name, sizeof(name)), key);
    t_snap(k, fmt, v);
}

void snap_bytes(const UsbDevice &d, const char *key, const uint8_t *p,
                uint32_t len)
{
    char name[32], k[64], hex[2 * 256 + 1];
    snprintf(k, sizeof(k), "%s.%s", dev_name(d, name, sizeof(name)), key);
    len = min(len, 256u);
    for (uint32_t i = 0; i < len; i++)
        snprintf(hex + 2 * i, 3, "%02x", p[i]);
    hex[2 * len] = 0;
    t_snap(k, "%s", hex);
}

uint32_t req(UsbDevice &d, uint8_t type, uint8_t request, uint16_t value,
             uint16_t index, uint16_t len = 0, void *buf = nullptr,
             uint32_t *actual = nullptr)
{
    uint32_t cc = d.Control(type, request, value, index, len, buf, actual);
    if (cc == CC_STALL)
        d.Recover(1);
    return cc;
}

/* The endpoints of the configuration, all interfaces, alternate 0. */
int endpoints(UsbDevice &d, UsbEndpoint *eps, int max)
{
    int n = 0;
    for (int i = 0; usb_find_interface(&d, i) != nullptr; i++) {
        for (int k = 0; n < max && d.FindEndpoint(i, k, &eps[n]); k++)
            n++;
    }
    return n;
}

}

/* 9.6.1, 9.6.3-9.6.6 */
TEST(usb, descriptors)
{
    int n = all_devices();
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        const uint8_t *dd = d.desc;
        snap_bytes(d, "device", dd, 18);
        snap_bytes(d, "config", d.config, d.config_len);
        EXPECT_EQ(dd[0], 18);
        EXPECT_EQ(dd[1], USB_DT_DEVICE);
        uint16_t bcd = le16(dd + 2);
        EXPECT_TRUE(bcd == 0x0110 || bcd == 0x0200 || bcd >= 0x0201);
        /* endpoint 0's packet size for the speed (5.5.3) */
        uint8_t mps0 = dd[7];
        if (d.speed == SPEED_LOW)
            EXPECT_EQ(mps0, 8);
        else if (d.speed == SPEED_FULL)
            EXPECT_TRUE(mps0 == 8 || mps0 == 16 || mps0 == 32 || mps0 == 64);
        else if (d.speed == SPEED_HIGH)
            EXPECT_EQ(mps0, 64);
        EXPECT_GE(dd[17], 1);

        const uint8_t *c = d.config;
        EXPECT_EQ(c[0], 9);
        EXPECT_EQ(c[1], USB_DT_CONFIG);
        EXPECT_EQ(le16(c + 2), d.config_len);
        EXPECT_GE(c[5], 1);
        /* D7 reserved, set to one; D4..D0 reserved, zero */
        EXPECT_EQ(c[7] & 0x9f, 0x80);
        /* the descriptors after it fill wTotalLength exactly, and each
           interface has the endpoints it says */
        uint32_t off = 9;
        int ifaces = 0, eps_left = 0;
        while (off + 2 <= d.config_len) {
            const uint8_t *p = c + off;
            ASSERT_GE(p[0], 2);
            if (p[1] == USB_DT_INTERFACE) {
                EXPECT_EQ(eps_left, 0);
                EXPECT_EQ(p[0], 9);
                if (p[3] == 0)
                    ifaces++;
                eps_left = p[4];
            } else if (p[1] == USB_DT_ENDPOINT) {
                EXPECT_TRUE(p[0] == 7 || p[0] == 9);
                EXPECT_GT(eps_left, 0);
                eps_left--;
                uint8_t num = p[2] & 0xf, type = p[3] & 3;
                uint16_t mps = le16(p + 4) & 0x7ff;
                EXPECT_NE(num, 0);
                EXPECT_EQ(p[2] & 0x70, 0);
                /* packet sizes and intervals for the speed (5.6-5.8) */
                if (type == 2) {
                    if (d.speed == SPEED_HIGH)
                        EXPECT_EQ(mps, 512);
                    else
                        EXPECT_TRUE(mps == 8 || mps == 16 || mps == 32 ||
                                    mps == 64);
                } else if (type == 3) {
                    if (d.speed == SPEED_HIGH) {
                        EXPECT_LE(mps, 1024);
                        EXPECT_GE(p[6], 1);
                        EXPECT_LE(p[6], 16);
                    } else {
                        EXPECT_LE(mps, d.speed == SPEED_LOW ? 8 : 64);
                        EXPECT_GE(p[6], 1);
                    }
                }
            }
            off += p[0];
        }
        EXPECT_EQ(off, (uint32_t)d.config_len);
        EXPECT_EQ(eps_left, 0);
        EXPECT_EQ(ifaces, c[4]);
    }
}

/* 9.4.3, 9.6.7 */
TEST(usb, string_descriptors)
{
    int n = all_devices();
    uint8_t *buf = (uint8_t *)usb_alloc(256);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        uint32_t got;
        /* index 0: the LANGIDs */
        ASSERT_EQ(d.GetDescriptor(USB_DT_STRING, 0, buf, 255, &got),
                  (uint32_t)CC_SUCCESS);
        EXPECT_GE(got, 4u);
        EXPECT_EQ(buf[0], got);
        EXPECT_EQ(buf[1], USB_DT_STRING);
        EXPECT_EQ(buf[0] % 2, 0);
        uint16_t lang = le16(buf + 2);
        snap_dev(d, "langid", "%#x", lang);
        /* the strings the device descriptor names */
        static const int kFields[] = {14, 15, 16};
        static const char *const kKeys[] = {"manufacturer", "product",
                                            "serial"};
        for (int f = 0; f < 3; f++) {
            uint8_t index = d.desc[kFields[f]];
            if (index == 0)
                continue;
            ASSERT_EQ(d.Control(USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                                USB_DT_STRING << 8 | index, lang, 255, buf,
                                &got), (uint32_t)CC_SUCCESS);
            EXPECT_EQ(buf[0], got);
            EXPECT_EQ(buf[1], USB_DT_STRING);
            EXPECT_EQ(buf[0] % 2, 0);
            char s[64];
            uint32_t len = 0;
            for (uint32_t k = 2; k + 1 < got && len + 1 < sizeof(s); k += 2)
                s[len++] = le16(buf + k) < 0x80 ? buf[k] : '?';
            s[len] = 0;
            char name[32], key[64];
            snprintf(key, sizeof(key), "%s.%s",
                     dev_name(d, name, sizeof(name)), kKeys[f]);
            t_snap(key, "%s", s);
            /* only the header */
            EXPECT_EQ(d.Control(USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                                USB_DT_STRING << 8 | index, lang, 2, buf,
                                &got), (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 2u);
        }
        /* one it does not have: a Request Error */
        EXPECT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                      USB_DT_STRING << 8 | 0x7f, lang, 255, buf),
                  (uint32_t)CC_STALL);
        EXPECT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                      USB_DT_STRING << 8 | 7, lang, 255, buf),
                  (uint32_t)CC_STALL);
    }
}

/* 9.4.3: descriptors a device may not have, and the high speed ones
   (9.6.2, 9.6.4) */
TEST(usb, other_descriptors)
{
    int n = all_devices();
    uint8_t *buf = (uint8_t *)usb_alloc(256);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        uint32_t got;
        /* a configuration index past bNumConfigurations */
        EXPECT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                      USB_DT_CONFIG << 8 | d.desc[17], 0, 255, buf),
                  (uint32_t)CC_STALL);
        /* the configuration header alone */
        EXPECT_EQ(d.GetDescriptor(USB_DT_CONFIG, 0, buf, 4, &got),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, 4u);
        /* the device descriptor, short */
        EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 1, &got),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, 1u);
        /* interface and endpoint descriptors are not fetched alone */
        EXPECT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                      USB_DT_INTERFACE << 8, 0, 255, buf),
                  (uint32_t)CC_STALL);
        EXPECT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                      USB_DT_ENDPOINT << 8, 0, 255, buf),
                  (uint32_t)CC_STALL);
        /* a high speed device says how it would be at full speed; a full
           speed only one refuses (9.6.2, 9.6.4) */
        uint32_t cc = req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                          USB_DT_QUALIFIER << 8, 0, 10, buf, &got);
        if (d.speed != SPEED_HIGH) {
            EXPECT_EQ(cc, (uint32_t)CC_STALL);
            EXPECT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                          USB_DT_OTHER_SPEED << 8, 0, 255, buf),
                      (uint32_t)CC_STALL);
        } else {
            ASSERT_EQ(cc, (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 10u);
            EXPECT_EQ(buf[0], 10);
            EXPECT_EQ(buf[1], USB_DT_QUALIFIER);
            EXPECT_EQ(le16(buf + 2), le16(d.desc + 2));
            EXPECT_EQ(memcmp(buf + 4, d.desc + 4, 3), 0);
            EXPECT_TRUE(buf[7] == 8 || buf[7] == 16 || buf[7] == 32 ||
                        buf[7] == 64);
            EXPECT_EQ(buf[8], d.desc[17]);
            EXPECT_EQ(buf[9], 0);
            ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                          USB_DT_OTHER_SPEED << 8, 0, 255, buf, &got),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(buf[1], USB_DT_OTHER_SPEED);
            EXPECT_EQ(got, (uint32_t)le16(buf + 2));
            EXPECT_EQ(memcmp(buf + 4, d.config + 4, 5), 0);
            for (uint32_t off = 0; off + 7 <= got && buf[off] >= 2;
                 off += buf[off]) {
                const uint8_t *p = buf + off;
                if (p[1] == USB_DT_ENDPOINT && (p[3] & 3) >= 2) {
                    EXPECT_LE(le16(p + 4) & 0x7ff, 64);
                    if ((p[3] & 3) == 3)
                        EXPECT_GE(p[6], 1);
                }
            }
            snap_bytes(d, "other_speed", buf, got);
        }
        snap_dev(d, "bos", "%u",
                 req(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                     USB_DT_BOS << 8, 0, 255, buf));
        /* SET_DESCRIPTOR is optional; snapshot */
        snap_dev(d, "set_descriptor", "%u",
                 req(d, 0, USB_REQ_SET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, 18,
                     d.desc));
    }
}

/* 9.4.2, 9.4.7, 9.1.1.5 */
TEST(usb, configuration)
{
    int n = all_devices();
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        uint8_t value = d.config[5];
        uint32_t got;
        /* a hub was configured to walk it */
        if (d.hub_ports == 0) {
            /* Address state: 0 */
            ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 1,
                          buf, &got), (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 1u);
            EXPECT_EQ(buf[0], 0);
        }
        EXPECT_EQ(req(d, 0, USB_REQ_SET_CONFIGURATION, value, 0),
                  (uint32_t)CC_SUCCESS);
        ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 1, buf),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(buf[0], value);
        /* a value no configuration has: a Request Error, and it stays */
        EXPECT_EQ(req(d, 0, USB_REQ_SET_CONFIGURATION, value + 5, 0),
                  (uint32_t)CC_STALL);
        ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 1, buf),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(buf[0], value);
        /* 0: back to Address */
        if (d.hub_ports == 0) {
            EXPECT_EQ(req(d, 0, USB_REQ_SET_CONFIGURATION, 0, 0),
                      (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 1,
                          buf), (uint32_t)CC_SUCCESS);
            EXPECT_EQ(buf[0], 0);
        }
    }
}

/* 9.4.4, 9.4.10 */
TEST(usb, interfaces)
{
    int n = all_devices();
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        int count = d.config[4];
        if (d.hub_ports == 0) {
            /* Address state: a Request Error */
            EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                          USB_REQ_GET_INTERFACE, 0, 0, 1, buf),
                      (uint32_t)CC_STALL);
            EXPECT_EQ(req(d, USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, 0, 0),
                      (uint32_t)CC_STALL);
            ASSERT_EQ(req(d, 0, USB_REQ_SET_CONFIGURATION, d.config[5], 0),
                      (uint32_t)CC_SUCCESS);
        }
        for (int k = 0; k < count; k++) {
            const uint8_t *id = usb_find_interface(&d, k);
            ASSERT_TRUE(id != nullptr);
            uint32_t got;
            ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                          USB_REQ_GET_INTERFACE, 0, id[2], 1, buf, &got),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 1u);
            EXPECT_EQ(buf[0], 0);
            EXPECT_EQ(req(d, USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, 0,
                          id[2]), (uint32_t)CC_SUCCESS);
            /* an alternate setting it does not have */
            bool has_alt = false;
            for (uint32_t off = 0; off + 2 <= d.config_len;
                 off += d.config[off]) {
                const uint8_t *p = d.config + off;
                has_alt |= p[1] == USB_DT_INTERFACE && p[2] == id[2] &&
                           p[3] == 1;
            }
            if (!has_alt)
                EXPECT_EQ(req(d, USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
                              1, id[2]), (uint32_t)CC_STALL);
            /* GET_STATUS of an interface: zero (9.4.5) */
            ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                          USB_REQ_GET_STATUS, 0, id[2], 2, buf, &got),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(got, 2u);
            EXPECT_EQ(le16(buf), 0);
        }
        /* an interface the configuration does not have */
        EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE,
                      USB_REQ_GET_INTERFACE, 0, count + 3, 1, buf),
                  (uint32_t)CC_STALL);
        EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_INTERFACE, USB_REQ_GET_STATUS,
                      0, count + 3, 2, buf), (uint32_t)CC_STALL);
    }
}

/* 9.4.1, 9.4.5, 9.4.9 */
TEST(usb, status_and_features)
{
    int n = all_devices();
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        uint8_t attrs = d.config[7];
        uint32_t got;
        ASSERT_EQ(req(d, 0, USB_REQ_SET_CONFIGURATION, d.config[5], 0),
                  (uint32_t)CC_SUCCESS);
        /* the device: Self Powered as the configuration says, Remote Wakeup
           off until set */
        ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_STATUS, 0, 0, 2, buf, &got),
                  (uint32_t)CC_SUCCESS);
        EXPECT_EQ(got, 2u);
        EXPECT_EQ(le16(buf) & 1, (attrs >> 6) & 1);
        EXPECT_EQ(le16(buf) & 2, 0);
        snap_dev(d, "status", "%#x", le16(buf));
        uint32_t cc = req(d, 0, USB_REQ_SET_FEATURE, FEATURE_REMOTE_WAKEUP, 0);
        if (attrs & 0x20) {
            EXPECT_EQ(cc, (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_STATUS, 0, 0, 2, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(le16(buf) & 2, 2);
            EXPECT_EQ(req(d, 0, USB_REQ_CLEAR_FEATURE, FEATURE_REMOTE_WAKEUP,
                          0), (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, USB_DIR_IN, USB_REQ_GET_STATUS, 0, 0, 2, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(le16(buf) & 2, 0);
        } else {
            EXPECT_EQ(cc, (uint32_t)CC_STALL);
        }
        /* a feature a device does not have */
        EXPECT_EQ(req(d, 0, USB_REQ_SET_FEATURE, 0x55, 0),
                  (uint32_t)CC_STALL);

        /* endpoints: Halt, set and cleared by the feature */
        UsbEndpoint eps[8];
        int ne = endpoints(d, eps, 8);
        ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT, USB_REQ_GET_STATUS,
                      0, 0, 2, buf, &got), (uint32_t)CC_SUCCESS);
        EXPECT_EQ(le16(buf), 0);
        for (int k = 0; k < ne; k++) {
            uint8_t a = eps[k].address;
            ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT,
                          USB_REQ_GET_STATUS, 0, a, 2, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(le16(buf), 0);
            EXPECT_EQ(req(d, USB_RECIP_ENDPOINT, USB_REQ_SET_FEATURE,
                          FEATURE_ENDPOINT_HALT, a), (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT,
                          USB_REQ_GET_STATUS, 0, a, 2, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(le16(buf), 1);
            EXPECT_EQ(req(d, USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                          FEATURE_ENDPOINT_HALT, a), (uint32_t)CC_SUCCESS);
            ASSERT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT,
                          USB_REQ_GET_STATUS, 0, a, 2, buf),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(le16(buf), 0);
        }
        /* an endpoint the configuration does not have */
        EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT, USB_REQ_GET_STATUS,
                      0, 0x8f, 2, buf), (uint32_t)CC_STALL);

        /* Address state: endpoints other than 0 are a Request Error */
        if (d.hub_ports == 0 && ne > 0) {
            ASSERT_EQ(req(d, 0, USB_REQ_SET_CONFIGURATION, 0, 0),
                      (uint32_t)CC_SUCCESS);
            EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT,
                          USB_REQ_GET_STATUS, 0, eps[0].address, 2, buf),
                      (uint32_t)CC_STALL);
            EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT,
                          USB_REQ_GET_STATUS, 0, 0, 2, buf),
                      (uint32_t)CC_SUCCESS);
        }
    }
}

/* 9.4.11 SYNCH_FRAME, for isochronous endpoints only; and a request code
   the standard does not define */
TEST(usb, unknown_requests)
{
    int n = all_devices();
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    for (int i = 0; i < n; i++) {
        UsbDevice &d = sDevs[i];
        EXPECT_EQ(req(d, USB_DIR_IN | USB_RECIP_ENDPOINT, 12, 0, 0x81, 2,
                      buf), (uint32_t)CC_STALL);
        EXPECT_EQ(req(d, USB_DIR_IN, 0x42, 0, 0, 2, buf),
                  (uint32_t)CC_STALL);
        EXPECT_EQ(req(d, USB_DIR_IN | USB_TYPE_VENDOR, 0x01, 0, 0, 2, buf),
                  (uint32_t)CC_STALL);
        /* and endpoint 0 works after each */
        EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 8),
                  (uint32_t)CC_SUCCESS);
    }
}

/* A port reset puts the device back in the Default state (9.1.1.3): it
   answers at address 0 again with its configuration gone. */
TEST(usb, bus_reset)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    if (d.route != 0)
        t_skip("behind a hub");
    Xhci *hc = d.hc;
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    ASSERT_TRUE(hc->PortReset(d.root_port));
    ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_RESET_DEVICE) |
                          TRB_SLOT(d.slot)), (uint32_t)CC_SUCCESS);
    memset(d.in, 0, 33 * hc->ctx_size);
    put_le32(d.InCtl() + 4, 0x3);
    memcpy(d.InSlot(), d.OutSlot(), hc->ctx_size);
    memcpy(d.InEp(1), d.OutEp(1), hc->ctx_size);
    put_le32(d.InEp(1), 0);
    put_le32(d.InEp(1) + 8, (uint32_t)d.rings[1].Enqueue() |
             (d.rings[1].cycle ? 1 : 0));
    ASSERT_EQ(hc->Command((uint32_t)(uintptr_t)d.in, 0, 0,
                          TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(d.slot)),
              (uint32_t)CC_SUCCESS);
    ASSERT_EQ(d.Control(USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 1, buf),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(buf[0], 0);
}
