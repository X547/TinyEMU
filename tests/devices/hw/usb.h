/*
 * USB through xHCI host controllers
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

#include "pci.h"
#include "registry.h"
#include "test.h"

/* xHCI host controllers as the eXtensible Host Controller Interface
   specification, revision 1.2, has them, and the USB devices behind them
   (USB 2.0, chapters 9 and 11). Section numbers below are the xHCI
   specification's unless they say otherwise. */

/* capability registers (5.3) */
#define XHCI_CAPLENGTH  0x00
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCSPARAMS3 0x0c
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF      0x14
#define XHCI_RTSOFF     0x18
#define XHCI_HCCPARAMS2 0x1c

/* operational registers (5.4), from CAPLENGTH */
#define XHCI_USBCMD     0x00
#define XHCI_USBSTS     0x04
#define XHCI_PAGESIZE   0x08
#define XHCI_DNCTRL     0x14
#define XHCI_CRCR       0x18
#define XHCI_DCBAAP     0x30
#define XHCI_CONFIG     0x38
#define XHCI_PORTSC(p)  (0x400 + 0x10 * ((p) - 1))
#define XHCI_PORTPMSC(p) (0x404 + 0x10 * ((p) - 1))
#define XHCI_PORTLI(p)  (0x408 + 0x10 * ((p) - 1))
#define XHCI_PORTHLPMC(p) (0x40c + 0x10 * ((p) - 1))

#define USBCMD_RS    0x0001
#define USBCMD_HCRST 0x0002
#define USBCMD_INTE  0x0004
#define USBCMD_HSEE  0x0008

#define USBSTS_HCH   0x0001
#define USBSTS_HSE   0x0004
#define USBSTS_EINT  0x0008
#define USBSTS_PCD   0x0010
#define USBSTS_CNR   0x0800
#define USBSTS_HCE   0x1000

#define CRCR_RCS 0x1
#define CRCR_CS  0x2
#define CRCR_CA  0x4
#define CRCR_CRR 0x8

#define PORTSC_CCS   (1u << 0)
#define PORTSC_PED   (1u << 1)
#define PORTSC_OCA   (1u << 3)
#define PORTSC_PR    (1u << 4)
#define PORTSC_PLS(v) (((v) >> 5) & 0xf)
#define PORTSC_PP    (1u << 9)
#define PORTSC_SPEED(v) (((v) >> 10) & 0xf)
#define PORTSC_LWS   (1u << 16)
#define PORTSC_CSC   (1u << 17)
#define PORTSC_PEC   (1u << 18)
#define PORTSC_WRC   (1u << 19)
#define PORTSC_OCC   (1u << 20)
#define PORTSC_PRC   (1u << 21)
#define PORTSC_PLC   (1u << 22)
#define PORTSC_CEC   (1u << 23)
#define PORTSC_WCE   (1u << 25)
#define PORTSC_WDE   (1u << 26)
#define PORTSC_WOE   (1u << 27)
#define PORTSC_WPR   (1u << 31)
#define PORTSC_CHANGES (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                        PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* what a write must give back to leave the port as it is (4.19.2) */
#define PORTSC_KEEP  (PORTSC_PP | (3u << 14) | PORTSC_WCE | PORTSC_WDE | \
                      PORTSC_WOE)

#define PLS_U0       0
#define PLS_U3       3
#define PLS_DISABLED 4
#define PLS_RXDETECT 5
#define PLS_POLLING  7

/* the default protocol speed IDs (7.2.2.1.1) */
#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4

/* runtime registers (5.5), interrupter 0 at 0x20 */
#define XHCI_MFINDEX 0x00
#define XHCI_IMAN    0x20
#define XHCI_IMOD    0x24
#define XHCI_ERSTSZ  0x28
#define XHCI_ERSTBA  0x30
#define XHCI_ERDP    0x38

#define IMAN_IP  0x1
#define IMAN_IE  0x2
#define ERDP_EHB 0x8

/* extended capabilities (7) */
#define XECP_LEGACY   1
#define XECP_PROTOCOL 2

/* TRB types (6.4.6) */
#define TRB_NORMAL     1
#define TRB_SETUP      2
#define TRB_DATA       3
#define TRB_STATUS     4
#define TRB_ISOCH      5
#define TRB_LINK       6
#define TRB_EVENT_DATA 7
#define TRB_NOOP       8
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define TRB_RESET_EP   14
#define TRB_STOP_EP    15
#define TRB_SET_TR_DEQ 16
#define TRB_RESET_DEVICE 17
#define TRB_FORCE_EVENT 18
#define TRB_NEGOTIATE_BW 19
#define TRB_SET_LT     20
#define TRB_GET_BW     21
#define TRB_FORCE_HEADER 22
#define TRB_NOOP_CMD   23
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND 33
#define TRB_EV_PORT    34
#define TRB_EV_BANDWIDTH 35
#define TRB_EV_DOORBELL 36
#define TRB_EV_HOST    37
#define TRB_EV_DEVICE  38
#define TRB_EV_MFINDEX 39

/* TRB control bits */
#define TRB_C    (1u << 0)
#define TRB_ENT  (1u << 1)
#define TRB_TC   (1u << 1)
#define TRB_ISP  (1u << 2)
#define TRB_CH   (1u << 4)
#define TRB_IOC  (1u << 5)
#define TRB_IDT  (1u << 6)
#define TRB_BSR  (1u << 9)
#define TRB_DC   (1u << 9)
#define TRB_BEI  (1u << 9)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3f)
#define TRB_SLOT(s) ((uint32_t)(s) << 24)
#define TRB_EP(e)   ((uint32_t)(e) << 16)
#define TRB_TRT_NONE 0
#define TRB_TRT_OUT  (2u << 16)
#define TRB_TRT_IN   (3u << 16)

/* completion codes (6.4.5) */
#define CC_INVALID         0
#define CC_SUCCESS         1
#define CC_DATA_BUFFER     2
#define CC_BABBLE          3
#define CC_TRANSACTION     4
#define CC_TRB             5
#define CC_STALL           6
#define CC_RESOURCE        7
#define CC_BANDWIDTH       8
#define CC_NO_SLOTS        9
#define CC_STREAM_TYPE     10
#define CC_SLOT_NOT_ENABLED 11
#define CC_EP_NOT_ENABLED  12
#define CC_SHORT_PACKET    13
#define CC_RING_UNDERRUN   14
#define CC_RING_OVERRUN    15
#define CC_VF_EVENT_FULL   16
#define CC_PARAMETER       17
#define CC_BANDWIDTH_OVERRUN 18
#define CC_CONTEXT_STATE   19
#define CC_NO_PING         20
#define CC_EVENT_RING_FULL 21
#define CC_INCOMPATIBLE    22
#define CC_MISSED_SERVICE  23
#define CC_CMD_RING_STOPPED 24
#define CC_CMD_ABORTED     25
#define CC_STOPPED         26
#define CC_STOPPED_LENGTH_INVALID 27
#define CC_STOPPED_SHORT   28
#define CC_TIMEOUT         0x100 /* the suite's own: no event came */

/* slot and endpoint states (6.2.2, 6.2.3) */
#define SLOT_DISABLED   0
#define SLOT_DEFAULT    1
#define SLOT_ADDRESSED  2
#define SLOT_CONFIGURED 3
#define EP_DISABLED 0
#define EP_RUNNING  1
#define EP_HALTED   2
#define EP_STOPPED  3
#define EP_ERROR    4

/* endpoint types (6.2.3) */
#define EPT_ISOCH_OUT 1
#define EPT_BULK_OUT  2
#define EPT_INTR_OUT  3
#define EPT_CONTROL   4
#define EPT_ISOCH_IN  5
#define EPT_BULK_IN   6
#define EPT_INTR_IN   7

/* USB 2.0 chapter 9 */
#define USB_DIR_IN     0x80
#define USB_TYPE_CLASS 0x20
#define USB_TYPE_VENDOR 0x40
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02
#define USB_RECIP_OTHER     0x03
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE  0x0a
#define USB_REQ_SET_INTERFACE  0x0b
#define USB_DT_DEVICE    1
#define USB_DT_CONFIG    2
#define USB_DT_STRING    3
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT  5
#define USB_DT_QUALIFIER 6
#define USB_DT_OTHER_SPEED 7
#define USB_DT_BOS       0x0f
#define USB_DT_HID       0x21
#define USB_DT_REPORT    0x22
#define USB_DT_HUB       0x29
#define USB_CLASS_HID     0x03
#define USB_CLASS_STORAGE 0x08
#define USB_CLASS_HUB     0x09

struct Trb {
    uint32_t d[4];   /* parameter low and high, status, control */

    uint64_t Param() const {return d[0] | (uint64_t)d[1] << 32;}
    uint32_t Type() const {return TRB_GET_TYPE(d[3]);}
    uint32_t Cc() const {return d[2] >> 24;}
    uint32_t Slot() const {return d[3] >> 24;}
    uint32_t Ep() const {return (d[3] >> 16) & 0x1f;}
    uint32_t Residual() const {return d[2] & 0xffffff;}
};

/* A producer ring of 'size' TRBs, the last of them a Link TRB back to the
   start that toggles the cycle (4.9.2). */
struct XhciRing {
    Trb *trbs = nullptr;
    uint32_t size = 0;
    uint32_t enq = 0;
    bool cycle = true;

    bool Init(uint32_t n);
    uint64_t Base() const {return (uintptr_t)trbs;}
    /* Write a TRB at the enqueue pointer, cycle bit last, and give its
       address. */
    uint64_t Push(uint32_t p0, uint32_t p1, uint32_t status, uint32_t control);
    uint64_t Push(const Trb &t) {return Push(t.d[0], t.d[1], t.d[2], t.d[3]);}
    uint64_t Enqueue() const {return Base() + 16 * enq;}
};

#define XHCI_EVENTS 64
#define XHCI_BACKLOG 64
#define XHCI_MAX_PORTS 32
#define XHCI_DCIS 32

class Xhci {
public:
    const HwDevice *dev = nullptr;  /* in the tables, or */
    PciFunc *fn = nullptr;          /* on PCI */
    uint64_t base = 0;

    uint32_t op = 0, rt = 0, db = 0;
    uint32_t max_slots = 0, max_intrs = 0, max_ports = 0;
    uint32_t ctx_size = 32;
    bool ac64 = false;

    uint64_t *dcbaa = nullptr;
    XhciRing cmd {};
    Trb *events = nullptr;
    uint32_t event_count = 0;
    uint32_t event_deq = 0;
    bool event_cycle = true;
    uint8_t *erst = nullptr;
    /* events read while waiting for another */
    Trb backlog[XHCI_BACKLOG];
    int backlog_count = 0;
    bool running = false;

    Xhci(const HwDevice *d, PciFunc *f);
    const char *Name() const {return fn != nullptr ? "xhci-pci" : "xhci";}

    uint32_t Rd(uint32_t off);
    void Wr(uint32_t off, uint32_t v);
    uint32_t OpRd(uint32_t off) {return Rd(op + off);}
    void OpWr(uint32_t off, uint32_t v) {Wr(op + off, v);}
    void OpWr64(uint32_t off, uint64_t v)
        {Wr(op + off, v); Wr(op + off + 4, v >> 32);}
    uint32_t RtRd(uint32_t off) {return Rd(rt + off);}
    void RtWr(uint32_t off, uint32_t v) {Wr(rt + off, v);}
    void RtWr64(uint32_t off, uint64_t v)
        {Wr(rt + off, v); Wr(rt + off + 4, v >> 32);}
    void Doorbell(uint32_t slot, uint32_t target);

    /* The capability registers into the fields above. */
    void ReadCaps();
    /* Halt, then reset and wait for it to finish; false when it did not. */
    bool Reset();
    /* Reset, give it the slot array, the command ring and an event ring of
       XHCI_EVENTS, and run it unless 'run' is false. */
    bool Init(bool run = true, uint32_t cmd_size = 32);
    bool Run();
    bool Halt();
    /* Halt and reset, for t_on_cleanup(). */
    void Cleanup();

    /* The next event, in order, taking the backlog first; false when none
       came in 'timeout_us'. */
    bool NextEvent(Trb *ev, uint64_t timeout_us = 1000000);
    /* An event that 'match' accepts, keeping the others for later. */
    template <typename F>
    bool WaitEvent(F match, Trb *ev, uint64_t timeout_us = 1000000);
    bool PollRing(Trb *ev);
    /* Forget everything waiting: the backlog and the ring. */
    void DrainEvents();
    /* Port status change events in the backlog for 'port', taken out. */
    int TakePortEvents(uint32_t port);

    /* A command and its completion: the completion code, or CC_TIMEOUT.
       'ev' gets the event. */
    uint32_t Command(uint32_t p0, uint32_t p1, uint32_t status,
                     uint32_t control, Trb *ev = nullptr,
                     uint64_t timeout_us = 1000000);
    uint32_t EnableSlot(uint32_t *slot);
    uint32_t DisableSlot(uint32_t slot);

    uint32_t Portsc(uint32_t port) {return OpRd(XHCI_PORTSC(port));}
    /* Write PORTSC leaving everything else as it is, plus 'set'. */
    void PortSet(uint32_t port, uint32_t set);
    /* Reset a port with something attached (4.3.1), clearing the change
       bits it left; false when it did not come back enabled. */
    bool PortReset(uint32_t port, bool warm = false);
    /* The major USB revision of a port, from the Supported Protocol
       capabilities, or 0. */
    int PortMajor(uint32_t port);
    /* The offset of the 'index'th extended capability with this ID, or 0. */
    uint32_t FindExtCap(uint8_t id, int index = 0);
};

template <typename F>
inline bool Xhci::WaitEvent(F match, Trb *ev, uint64_t timeout_us)
{
    for (int i = 0; i < backlog_count; i++) {
        if (match(backlog[i])) {
            *ev = backlog[i];
            backlog_count--;
            memmove(backlog + i, backlog + i + 1,
                    (backlog_count - i) * sizeof(Trb));
            return true;
        }
    }
    return t_wait_until([&] {
        Trb t;
        while (PollRing(&t)) {
            if (match(t)) {
                *ev = t;
                return true;
            }
            if (backlog_count == XHCI_BACKLOG) {
                backlog_count--;
                memmove(backlog, backlog + 1, backlog_count * sizeof(Trb));
            }
            backlog[backlog_count++] = t;
        }
        return false;
    }, timeout_us);
}

/* The hosts the tables and PCI give, found afresh for every test and put
   back after it. */
int xhci_count();
Xhci *xhci_host(int index);
/* The first host that initializes, or the test is skipped. */
Xhci *xhci_require(int index = 0);

struct UsbEndpoint {
    uint8_t address;     /* with USB_DIR_IN */
    uint8_t attributes;
    uint16_t max_packet;
    uint8_t interval;
};

#define USB_MAX_EPS 8
#define USB_MAX_TD_TRBS 8

/* A device addressed through a host. */
struct UsbDevice {
    Xhci *hc = nullptr;
    uint32_t slot = 0;
    uint32_t root_port = 0;
    uint32_t route = 0;
    uint32_t speed = 0;   /* protocol speed ID */
    uint32_t tt_slot = 0, tt_port = 0;
    uint8_t *in = nullptr;     /* input context */
    uint8_t *out = nullptr;    /* output device context */
    XhciRing rings[XHCI_DCIS];
    uint8_t desc[18];
    uint8_t *config = nullptr; /* the whole configuration */
    uint16_t config_len = 0;
    uint8_t hub_ports = 0;     /* a hub: set before Configure() */
    /* the TRBs of the last TD Queue() put on each ring */
    uint64_t td_trb[XHCI_DCIS][USB_MAX_TD_TRBS];
    uint32_t td_len[XHCI_DCIS][USB_MAX_TD_TRBS];
    int td_count[XHCI_DCIS] = {};

    uint8_t *InCtl() {return in;}
    uint8_t *InSlot() {return in + hc->ctx_size;}
    uint8_t *InEp(int dci) {return in + hc->ctx_size * (dci + 1);}
    uint8_t *OutSlot() {return out;}
    uint8_t *OutEp(int dci) {return out + hc->ctx_size * dci;}
    uint32_t SlotState();
    uint32_t EpState(int dci);

    /* Enable a slot and address the device on it (4.3.2-4.3.4), BSR as
       given; the endpoint 0 ring and the contexts come from the heap. */
    uint32_t Address(bool bsr = false);
    /* A control transfer, polled: the completion code of the last event,
       and 'actual' the bytes moved. */
    uint32_t Control(uint8_t type, uint8_t req, uint16_t value,
                     uint16_t index, uint16_t len, void *buf,
                     uint32_t *actual = nullptr);
    uint32_t GetDescriptor(uint8_t type, uint8_t index, void *buf,
                           uint16_t len, uint32_t *actual = nullptr);
    /* The device and configuration descriptors into 'desc' and 'config',
       fixing up endpoint 0's packet size with Evaluate Context. */
    bool ReadDescriptors();
    /* The 'n'th endpoint of interface 'iface', or false. */
    bool FindEndpoint(int iface, int n, UsbEndpoint *ep);
    /* SET_CONFIGURATION and a Configure Endpoint command adding every
       endpoint of the configuration's interfaces (alternate setting 0). */
    uint32_t Configure(uint8_t value = 1);
    /* Fill in the input context for one endpoint, with a new ring. */
    void SetupEpContext(const UsbEndpoint &ep);

    /* Queue a TD of Normal TRBs on endpoint 'dci' and ring it, and the
       first TRB's address; WaitTransfer() takes its event. */
    uint64_t Queue(int dci, void *buf, uint32_t len,
                   uint32_t flags = TRB_ISP | TRB_IOC);
    uint32_t WaitTransfer(int dci, uint32_t *residual = nullptr,
                          uint64_t timeout_us = 2000000, Trb *ev = nullptr);
    /* Both: the completion code, and 'actual' the bytes moved. */
    uint32_t Transfer(int dci, void *buf, uint32_t len,
                      uint32_t *actual = nullptr,
                      uint64_t timeout_us = 2000000);

    uint32_t StopEndpoint(int dci, Trb *ev = nullptr);
    uint32_t ResetEndpoint(int dci);
    /* Set TR Dequeue to the ring's enqueue pointer, skipping what is
       queued. */
    uint32_t SkipQueued(int dci);
    /* Halted endpoint back to running: Reset Endpoint, Set TR Dequeue
       past what was queued, CLEAR_FEATURE(ENDPOINT_HALT). */
    bool Recover(int dci);
};

inline int usb_dci(uint8_t address)
{
    return (address & 0xf) * 2 + ((address & USB_DIR_IN) ? 1 : 0);
}

/* The 'index'th device of interface class 'cls' (0 for any) on any host,
   descending through hubs, addressed and with its descriptors read but not
   configured; false when there is none. 'host' limits it to one host. */
bool usb_find_device(uint8_t cls, int index, UsbDevice *dev,
                     int host = -1);
/* Every device, hubs and what is behind them included, in the same order;
   how many were found. */
int usb_find_all(UsbDevice *devs, int max, int host = -1);
/* The same, or the test is skipped. */
void usb_require_device(uint8_t cls, int index, UsbDevice *dev,
                        int host = -1);
/* The 'n'th interface descriptor in a configuration, or nullptr. */
const uint8_t *usb_find_interface(const UsbDevice *dev, int n);

/* Under 4 GB, where a controller without 64 bit addressing reaches. */
void *usb_alloc(size_t size, size_t align = 64);

void usb_cleanup();

/* Mass storage, Bulk-Only Transport (USB Mass Storage Class Bulk-Only
   Transport 1.0): a command block wrapper out, the data, a command status
   wrapper in. */
#define BOT_PASS  0
#define BOT_FAIL  1
#define BOT_PHASE 2
#define BOT_TRANSPORT 0xff /* no valid status wrapper came */

struct UsbBot {
    UsbDevice dev;
    uint8_t iface = 0;
    int in = 0, out = 0;     /* the bulk endpoints' DCIs */
    uint32_t tag = 0x1000;
    uint8_t *cbw = nullptr;
    uint8_t *csw = nullptr;

    /* The 'index'th mass storage device, configured; false when none. */
    bool Open(int index);
    /* One command. 'in' gives the direction of a data stage of 'len'
       bytes; the status wrapper's status, or BOT_TRANSPORT. */
    uint32_t Command(uint8_t lun, const uint8_t *cdb, int cdb_len, bool in,
                     void *buf, uint32_t len, uint32_t *residue = nullptr);
    /* Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) on
       both endpoints, with the controller's side recovered too (5.3.4). */
    bool ResetRecovery();
    /* GET_MAX_LUN, or -1 when it stalled. */
    int MaxLun();
};
