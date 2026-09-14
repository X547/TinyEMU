/*
 * eXtensible Host Controller Interface (USB 3.0)
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
#include "xhci.h"

#include <stdio.h>
#include <string.h>

#include "cutils.h"
#include "host_time.h"
#include "machine.h"
#include "pci.h"
#include "usb.h"

//#define DEBUG_XHCI

#ifdef DEBUG_XHCI
#define xhci_debug(...) fprintf(stderr, "xhci: " __VA_ARGS__)
#else
#define xhci_debug(...) do {} while (0)
#endif


//#pragma mark - register map

/* The BAR is laid out the way QEMU lays its own out, which is the
   best-travelled path through both target guests' drivers. */
#define XHCI_CAP_LENGTH     0x40
#define XHCI_EXTCAP_OFFSET  0x20
#define XHCI_OPER_OFFSET    0x40
#define XHCI_PORT_OFFSET    0x440
#define XHCI_PORT_STRIDE    0x10
#define XHCI_RUNTIME_OFFSET 0x1000
#define XHCI_INTR_OFFSET    0x1020
#define XHCI_DOORBELL_OFFSET 0x2000
#define XHCI_MSIX_TABLE_OFFSET 0x3000
#define XHCI_MSIX_PBA_OFFSET   0x3800
#define XHCI_BAR_SIZE       0x4000

/* Capability registers. */
#define XHCI_CAPLENGTH  0x00
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCSPARAMS3 0x0c
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF      0x14
#define XHCI_RTSOFF     0x18
#define XHCI_HCCPARAMS2 0x1c

/* Haiku refuses to attach to a controller whose version falls outside
   [0x0090, 0x0120], and Linux reads HCCPARAMS2 only above 0x0100. */
#define XHCI_HCIVERSION 0x0100

#define XHCI_MAX_SLOTS 32

/* Operational registers, relative to XHCI_OPER_OFFSET. */
#define XHCI_USBCMD     0x00
#define XHCI_USBSTS     0x04
#define XHCI_PAGESIZE   0x08
#define XHCI_DNCTRL     0x14
#define XHCI_CRCR_LO    0x18
#define XHCI_CRCR_HI    0x1c
#define XHCI_DCBAAP_LO  0x30
#define XHCI_DCBAAP_HI  0x34
#define XHCI_CONFIG     0x38

#define USBCMD_RS       (1 << 0)
#define USBCMD_HCRST    (1 << 1)
#define USBCMD_INTE     (1 << 2)
#define USBCMD_HSEE     (1 << 3)
#define USBCMD_LHCRST   (1 << 7)
#define USBCMD_CSS      (1 << 8)
#define USBCMD_CRS      (1 << 9)
#define USBCMD_EWE      (1 << 10)
#define USBCMD_EU3S     (1 << 11)
#define USBCMD_CME      (1 << 13)

#define USBSTS_HCH      (1 << 0)
#define USBSTS_HSE      (1 << 2)
#define USBSTS_EINT     (1 << 3)
#define USBSTS_PCD      (1 << 4)
#define USBSTS_CNR      (1 << 11)

#define CRCR_RCS        (1 << 0)
#define CRCR_CS         (1 << 1)
#define CRCR_CA         (1 << 2)
#define CRCR_CRR        (1 << 3)

/* Interrupter registers, relative to XHCI_INTR_OFFSET. */
#define XHCI_IMAN       0x00
#define XHCI_IMOD       0x04
#define XHCI_ERSTSZ     0x08
#define XHCI_ERSTBA_LO  0x10
#define XHCI_ERSTBA_HI  0x14
#define XHCI_ERDP_LO    0x18
#define XHCI_ERDP_HI    0x1c

#define IMAN_IP         (1 << 0)
#define IMAN_IE         (1 << 1)
#define ERDP_EHB        (1 << 3)

/* Port status and control. The change bits are all write-one-to-clear; the
   reset bits are write-one-to-set and read back as zero because a reset here
   completes inside the write that started it. */
#define PORTSC_CCS      (1 << 0)
#define PORTSC_PED      (1 << 1)
#define PORTSC_OCA      (1 << 3)
#define PORTSC_PR       (1 << 4)
#define PORTSC_PLS_SHIFT 5
#define PORTSC_PLS_MASK (0xf << 5)
#define PORTSC_PP       (1 << 9)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK (0xf << 10)
#define PORTSC_LWS      (1 << 16)
#define PORTSC_CSC      (1 << 17)
#define PORTSC_PEC      (1 << 18)
#define PORTSC_WRC      (1 << 19)
#define PORTSC_OCC      (1 << 20)
#define PORTSC_PRC      (1 << 21)
#define PORTSC_PLC      (1 << 22)
#define PORTSC_CEC      (1 << 23)
#define PORTSC_WCE      (1 << 25)
#define PORTSC_WDE      (1 << 26)
#define PORTSC_WOE      (1 << 27)
#define PORTSC_WPR      (1u << 31)

#define PORTSC_CHANGE_MASK (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
                            PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

/* Port link states. */
#define PLS_U0          0
#define PLS_U3          3
#define PLS_DISABLED    4
#define PLS_RX_DETECT   5
#define PLS_POLLING     7

/* Protocol speed IDs, as the default table defines them. */
#define PORT_SPEED_FULL  1
#define PORT_SPEED_LOW   2
#define PORT_SPEED_HIGH  3
#define PORT_SPEED_SUPER 4

/* TRB types. */
#define TR_NORMAL       1
#define TR_SETUP        2
#define TR_DATA         3
#define TR_STATUS       4
#define TR_ISOCH        5
#define TR_LINK         6
#define TR_EVDATA       7
#define TR_NOOP         8
#define CR_ENABLE_SLOT  9
#define CR_DISABLE_SLOT 10
#define CR_ADDRESS_DEVICE 11
#define CR_CONFIGURE_ENDPOINT 12
#define CR_EVALUATE_CONTEXT 13
#define CR_RESET_ENDPOINT 14
#define CR_STOP_ENDPOINT 15
#define CR_SET_TR_DEQUEUE 16
#define CR_RESET_DEVICE 17
#define CR_NOOP         23
#define ER_TRANSFER     32
#define ER_COMMAND_COMPLETE 33
#define ER_PORT_STATUS_CHANGE 34

/* TRB control field. */
#define TRB_C           (1 << 0)
#define TRB_TR_ENT      (1 << 1)
#define TRB_TR_ISP      (1 << 2)
#define TRB_TR_CH       (1 << 4)
#define TRB_TR_IOC      (1 << 5)
#define TRB_TR_IDT      (1 << 6)
#define TRB_TR_BEI      (1 << 9)
#define TRB_LK_TC       (1 << 1)
#define TRB_EV_ED       (1 << 2)
#define TRB_CR_BSR      (1 << 9)
#define TRB_CR_DC       (1 << 9)
#define TRB_TYPE_SHIFT  10
#define TRB_TYPE(c)     (((c) >> TRB_TYPE_SHIFT) & 0x3f)

#define TRB_LEN_MASK    0x1ffff

/* Completion codes. */
#define CC_SUCCESS      1
#define CC_DATA_BUFFER_ERROR 2
#define CC_BABBLE       3
#define CC_USB_TRANSACTION_ERROR 4
#define CC_TRB_ERROR    5
#define CC_STALL        6
#define CC_NO_SLOTS     9
#define CC_SLOT_NOT_ENABLED 11
#define CC_EP_NOT_ENABLED 12
#define CC_SHORT_PACKET 13
#define CC_PARAMETER_ERROR 17
#define CC_CONTEXT_STATE_ERROR 19
#define CC_COMMAND_RING_STOPPED 24
#define CC_STOPPED      26
#define CC_STOPPED_LENGTH_INVALID 27

/* Slot states, in the top five bits of slot context dword 3. */
#define SLOT_DISABLED   0
#define SLOT_DEFAULT    1
#define SLOT_ADDRESSED  2
#define SLOT_CONFIGURED 3

/* Endpoint states, in the low three bits of endpoint context dword 0. */
#define EP_DISABLED     0
#define EP_RUNNING      1
#define EP_HALTED       2
#define EP_STOPPED      3
#define EP_ERROR        4

/* Endpoint types, in endpoint context dword 1 bits 5:3. */
#define EP_TYPE_ISOCH_OUT 1
#define EP_TYPE_BULK_OUT  2
#define EP_TYPE_INTR_OUT  3
#define EP_TYPE_CONTROL   4
#define EP_TYPE_ISOCH_IN  5
#define EP_TYPE_BULK_IN   6
#define EP_TYPE_INTR_IN   7

/* One context is 32 bytes: HCCPARAMS1.CSZ is zero. */
#define XHCI_CTX_SIZE   0x20

/* A transfer descriptor longer than this is refused rather than overrun. The
   longest a guest builds here is one scatter list entry per page of a bulk
   transfer, which is far below this. */
#define XHCI_MAX_TRBS_PER_TD 256

/* A Link TRB chain longer than this is a ring a guest built as a loop; the
   walk stops rather than hanging the emulator inside an MMIO write. */
#define XHCI_MAX_LINK_HOPS 32

/* Events that arrive with the event ring full wait here until the guest frees
   space, which it does by writing ERDP. */
#define XHCI_EVENT_FIFO_SIZE 256

#define XHCI_DCI_COUNT 32


//#pragma mark - structures

struct XHCITRB {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
    uint64_t addr; /* where the TRB was read from, for the Transfer Event */
};


struct XHCIRing {
    uint64_t deq = 0;
    bool ccs = true;
};


struct XHCIEvent {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};


class XHCIDevice;
struct XHCIEndpoint;


/* The payload of one transfer descriptor, described over the TRBs the guest
   chained together. Nothing below this ever sees a guest physical address, and
   the page-at-a-time chunking every access needs lives here alone. */
class XHCITransferBuffer final: public DataBuffer {
private:
    XHCIDevice *fXhci = nullptr;
    const XHCITRB *fTrbs = nullptr;
    int fCount = 0;
    uint32_t fLength = 0;

    uint32_t Move(uint32_t offset, void *host, uint32_t len, bool to_guest);

public:
    void Init(XHCIDevice *xhci, const XHCITRB *trbs, int count);

    uint32_t Length() const override {return fLength;}

    uint32_t Read(uint32_t offset, void *dst, uint32_t len) override
    {
        return Move(offset, dst, len, false);
    }

    uint32_t Write(uint32_t offset, const void *src, uint32_t len) override
    {
        return Move(offset, const_cast<void *>(src), len, true);
    }
};


/* One transfer descriptor in flight. It is embedded in its endpoint, because
   an endpoint can only ever have one: a transfer the device answered
   asynchronously blocks the ring until it completes. */
struct XHCITransfer {
    XHCITRB trbs[XHCI_MAX_TRBS_PER_TD];
    int trb_count = 0;

    /* The ring position the descriptor started at, so that Stop Endpoint can
       rewind to it and the guest can restart the transfer. */
    uint64_t snap_deq = 0;
    bool snap_ccs = true;

    URB urb {};
    XHCITransferBuffer buffer {};
    uint32_t cc = CC_SUCCESS;
    uint32_t actual_length = 0;
};


struct XHCIEndpoint final: public URBCompletion {
    XHCIDevice *xhci = nullptr;
    int slot_id = 0;
    int dci = 0;

    uint8_t state = EP_DISABLED;
    uint8_t type = 0;
    uint16_t max_packet = 0;
    XHCIRing ring {};

    bool busy = false;   /* a transfer is with the device */
    bool kicking = false;
    XHCITransfer xfer {};

    void Complete(URB *urb) override;
};


struct XHCISlot {
    bool enabled = false;
    uint8_t port = 0;      /* root hub port, 1 based */
    USBDevice *dev = nullptr;
    XHCIEndpoint *ep[XHCI_DCI_COUNT] {};
};


struct XHCIRootPort {
    USBPort port {};
    uint32_t portsc = 0;
    bool is_super = false;
};


//#pragma mark - XHCIDevice

class XHCIDevice final: public Device, public PCIBarTarget, public DeviceIO,
                        public USBPortTarget {
private:
    int fUsb2Ports;
    int fUsb3Ports;
    int fPortCount;

    PCIDevice *fPciDev = nullptr;
    PhysMemoryRange *fMemRange = nullptr;
    IRQSignal *fIrq = nullptr;
    PCIMsixState fMsix {};
    bool fMsixSent = false;

    USBBus *fChildBus = nullptr;

    /* operational state */
    uint32_t fUsbCmd = 0;
    uint32_t fUsbSts = USBSTS_HCH;
    uint32_t fDnCtrl = 0;
    uint32_t fConfig = 0;
    uint32_t fCrcrLo = 0;
    uint32_t fCrcrHi = 0;
    uint64_t fDcbaap = 0;
    uint32_t fDcbaapLo = 0;

    XHCIRing fCmdRing {};
    bool fInCmdRing = false;

    /* interrupter 0, the only one HCSPARAMS1 advertises */
    uint32_t fIman = 0;
    uint32_t fImod = 0;
    uint32_t fErstSz = 0;
    uint32_t fErstBaLo = 0;
    uint64_t fErstBa = 0;
    uint32_t fErdpLo = 0;
    uint32_t fErdpHi = 0;

    int fErSegIndex = 0;
    uint32_t fErSegOffset = 0;
    uint64_t fErSegStart = 0;
    uint32_t fErSegSize = 0;
    uint64_t fErEnqueue = 0;
    bool fErPcs = true;

    XHCIEvent fEventFifo[XHCI_EVENT_FIFO_SIZE] {};
    int fEventFifoCount = 0;

    XHCIRootPort fPorts[XHCI_MAX_PORTS] {};
    XHCISlot fSlots[XHCI_MAX_SLOTS + 1] {}; /* slot ids are 1 based */

    uint64_t fStartUs = 0;

    /* register file */
    uint32_t CapRead(uint32_t offset);
    uint32_t ExtCapRead(uint32_t offset);
    uint32_t OperRead(uint32_t offset);
    void OperWrite(uint32_t offset, uint32_t val);
    uint32_t PortRead(uint32_t offset);
    void PortWrite(uint32_t offset, uint32_t val);
    uint32_t RuntimeRead(uint32_t offset);
    void RuntimeWrite(uint32_t offset, uint32_t val);
    void DoorbellWrite(uint32_t offset, uint32_t val);
    uint32_t ReadDword(uint32_t offset);
    void WriteDword(uint32_t offset, uint32_t val);

    /* controller */
    void Reset();
    void SetRunning(bool running);
    uint32_t MfIndex() const;

    /* interrupts */
    void IntrRaise();
    void IntrUpdate();

    /* event ring */
    void LoadErSegment(int index);
    void ResetEventRing();
    bool EventRingFull() const;
    void AdvanceEventEnqueue();
    void PostEvent(const XHCIEvent &ev, bool bei);
    void DrainEventFifo();
    void PostCommandComplete(uint64_t cmd_addr, int cc, int slot_id);

    /* rings */
    int RingFetch(XHCIRing *ring, XHCITRB *trb);

    /* ports */
    void PortUpdate(int index);
    void PortReset(int index, bool warm);
    void PortNotify(int index, uint32_t bit);
    void PostPortEvent(int index);

    /* contexts */
    uint64_t SlotCtxAddr(int slot_id);
    uint64_t EpCtxAddr(int slot_id, int dci);
    uint32_t CtxRead(uint64_t base, int dword);
    void CtxWrite(uint64_t base, int dword, uint32_t val);
    void SetSlotState(int slot_id, int state);
    void SetEpState(int slot_id, int dci, int state);

    /* commands */
    void RunCommandRing();
    void ExecuteCommand(uint64_t addr, const XHCITRB &trb);
    int CmdEnableSlot(int *pslot_id);
    int CmdDisableSlot(int slot_id);
    int CmdAddressDevice(int slot_id, const XHCITRB &trb);
    int CmdConfigureEndpoint(int slot_id, const XHCITRB &trb);
    int CmdEvaluateContext(int slot_id, const XHCITRB &trb);
    int CmdResetEndpoint(int slot_id, int dci);
    int CmdStopEndpoint(int slot_id, int dci);
    int CmdSetTrDequeue(int slot_id, int dci, const XHCITRB &trb);
    int CmdResetDevice(int slot_id);

    XHCIEndpoint *GetEndpoint(int slot_id, int dci);
    XHCIEndpoint *EnableEndpoint(int slot_id, int dci);
    void DisableEndpoint(int slot_id, int dci);
    USBDevice *ResolveDevice(int port, uint32_t route);

    /* transfers */
    int FetchTD(XHCIEndpoint *ep);
    void XferReport(XHCIEndpoint *ep);
    void HaltEndpoint(XHCIEndpoint *ep);

public:
    XHCIDevice(const char *name, int usb2_ports, int usb3_ports);
    ~XHCIDevice() override;

    /* DMA, page at a time, in the shape virtio_memcpy_from_ram uses. */
    bool DmaRead(uint64_t addr, void *buf, uint32_t len);
    bool DmaWrite(uint64_t addr, const void *buf, uint32_t len);

    void KickEndpoint(XHCIEndpoint *ep);
    void EndpointComplete(XHCIEndpoint *ep, URB *urb);

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}

    void SetBar(int bar_num, uint64_t addr, bool enabled) override;
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    int FindFreePort(USBSpeedEnum speed) override;
    bool AttachDevice(USBDevice *dev, int port) override;
};


XHCIDevice::XHCIDevice(const char *name, int usb2_ports, int usb3_ports):
    Device(name), fUsb2Ports(usb2_ports), fUsb3Ports(usb3_ports)
{
    fPortCount = fUsb2Ports + fUsb3Ports;

    /* The SuperSpeed set comes first, matching the Supported Protocol
       capabilities published below. */
    for (int i = 0; i < fPortCount; i++) {
        fPorts[i].port.index = i + 1;
        fPorts[i].is_super = i < fUsb3Ports;
    }

    fStartUs = host_monotonic_us();
}


XHCIDevice::~XHCIDevice()
{
    delete fChildBus;
    for (int i = 1; i <= XHCI_MAX_SLOTS; i++) {
        for (int dci = 0; dci < XHCI_DCI_COUNT; dci++) {
            delete fSlots[i].ep[dci];
        }
    }
}


//#pragma mark - DMA

bool XHCIDevice::DmaRead(uint64_t addr, void *buf, uint32_t len)
{
    uint8_t *dst = static_cast<uint8_t *>(buf);

    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (addr & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, addr, false);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(dst, ptr, l);
        addr += l;
        dst += l;
        len -= l;
    }
    return true;
}


bool XHCIDevice::DmaWrite(uint64_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = static_cast<const uint8_t *>(buf);

    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (addr & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, addr, true);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(ptr, src, l);
        addr += l;
        src += l;
        len -= l;
    }
    return true;
}


//#pragma mark - XHCITransferBuffer

void XHCITransferBuffer::Init(XHCIDevice *xhci, const XHCITRB *trbs, int count)
{
    fXhci = xhci;
    fTrbs = trbs;
    fCount = count;
    fLength = 0;
    for (int i = 0; i < count; i++) {
        uint32_t type = TRB_TYPE(trbs[i].control);
        if (type == TR_NORMAL || type == TR_DATA || type == TR_ISOCH) {
            fLength += trbs[i].status & TRB_LEN_MASK;
        }
    }
}


uint32_t XHCITransferBuffer::Move(uint32_t offset, void *host, uint32_t len,
                                  bool to_guest)
{
    uint8_t *hp = static_cast<uint8_t *>(host);
    uint32_t pos = 0;  /* offset of the current TRB's data within the whole */
    uint32_t done = 0;

    for (int i = 0; i < fCount && len > 0; i++) {
        const XHCITRB &trb = fTrbs[i];
        uint32_t type = TRB_TYPE(trb.control);
        if (type != TR_NORMAL && type != TR_DATA && type != TR_ISOCH) {
            continue;
        }
        uint32_t chunk = trb.status & TRB_LEN_MASK;
        if (pos + chunk <= offset) {
            pos += chunk;
            continue;
        }

        uint32_t skip = offset - pos;
        uint32_t n = chunk - skip;
        if (n > len) {
            n = len;
        }

        if ((trb.control & TRB_TR_IDT) != 0) {
            /* Immediate data: the payload is the parameter field itself, so
               there is no guest buffer to write back into. */
            uint8_t imm[8];
            put_le64(imm, trb.parameter);
            if (to_guest || skip + n > 8) {
                return done;
            }
            memcpy(hp, imm + skip, n);
        } else {
            bool ok = to_guest
                ? fXhci->DmaWrite(trb.parameter + skip, hp, n)
                : fXhci->DmaRead(trb.parameter + skip, hp, n);
            if (!ok) {
                return done;
            }
        }

        pos += chunk;
        offset += n;
        hp += n;
        len -= n;
        done += n;
    }
    return done;
}


//#pragma mark - interrupts

void XHCIDevice::IntrRaise()
{
    /* Linux's handler returns IRQ_NONE unless EINT is set, so an interrupt
       without it is an interrupt the guest throws away. */
    fUsbSts |= USBSTS_EINT;
    if ((fIman & IMAN_IP) == 0) {
        fIman |= IMAN_IP;
        fErdpLo |= ERDP_EHB;
    }
    IntrUpdate();
}


void XHCIDevice::IntrUpdate()
{
    bool level = (fUsbCmd & USBCMD_INTE) != 0 && (fIman & IMAN_IE) != 0 &&
                 (fIman & IMAN_IP) != 0;

    if (fMsix.Enabled()) {
        /* A message is an edge, so it is sent once per assertion and rearmed
           when the guest drops the condition. */
        if (level && !fMsixSent) {
            fMsix.Send(0);
            fMsixSent = true;
        } else if (!level) {
            fMsixSent = false;
        }
        if (fIrq != nullptr) {
            fIrq->Set(0);
        }
        return;
    }
    if (fIrq != nullptr) {
        fIrq->Set(level ? 1 : 0);
    }
}


//#pragma mark - event ring

void XHCIDevice::LoadErSegment(int index)
{
    uint8_t entry[16];

    if (fErstBa == 0 || !DmaRead(fErstBa + (uint64_t)index * 16, entry, 16)) {
        fErSegStart = 0;
        fErSegSize = 0;
        fErEnqueue = 0;
        return;
    }
    /* The table is re-read on every segment load rather than cached, so a
       guest that rewrites it in place is followed for free. */
    fErSegStart = ((uint64_t)get_le32(entry + 4) << 32) | get_le32(entry);
    fErSegStart &= ~0x3fULL;
    fErSegSize = get_le16(entry + 8);
    if (fErSegSize > 4096) {
        fErSegSize = 4096;
    }
    fErEnqueue = fErSegStart + (uint64_t)fErSegOffset * 16;
}


void XHCIDevice::ResetEventRing()
{
    fErSegIndex = 0;
    fErSegOffset = 0;
    fErPcs = true;
    LoadErSegment(0);
}


void XHCIDevice::AdvanceEventEnqueue()
{
    fErSegOffset++;
    if (fErSegOffset >= fErSegSize) {
        fErSegIndex++;
        if ((uint32_t)fErSegIndex >= fErstSz) {
            fErSegIndex = 0;
            fErPcs = !fErPcs;
        }
        fErSegOffset = 0;
        LoadErSegment(fErSegIndex);
        return;
    }
    fErEnqueue = fErSegStart + (uint64_t)fErSegOffset * 16;
}


bool XHCIDevice::EventRingFull() const
{
    if (fErSegSize == 0) {
        return true;
    }
    /* The ring is full when the slot the next event would take is the one the
       guest has yet to consume. */
    uint32_t next = fErSegOffset + 1;
    uint64_t next_addr;
    if (next >= fErSegSize) {
        /* Only the wrap within this segment is checked; with more than one
           segment the guest is far from the boundary in practice. */
        next_addr = fErSegStart;
    } else {
        next_addr = fErSegStart + (uint64_t)next * 16;
    }
    uint64_t erdp = ((uint64_t)fErdpHi << 32) | (fErdpLo & ~0xfULL);
    return next_addr == erdp;
}


void XHCIDevice::PostEvent(const XHCIEvent &ev, bool bei)
{
    if (fErstSz == 0 || fErSegSize == 0) {
        return;
    }
    if (EventRingFull()) {
        /* Transfers here complete without back pressure, so an event that
           cannot be placed waits rather than being lost. */
        if (fEventFifoCount >= XHCI_EVENT_FIFO_SIZE) {
            vm_error("xhci: event ring and overflow queue both full\n");
            fUsbSts |= USBSTS_HSE;
            return;
        }
        fEventFifo[fEventFifoCount++] = ev;
        return;
    }

    uint8_t buf[16];
    put_le64(buf, ev.parameter);
    put_le32(buf + 8, ev.status);
    /* The cycle bit publishes the entry, so the control dword goes last. */
    put_le32(buf + 12, (ev.control & ~1u) | (fErPcs ? 1 : 0));
    if (!DmaWrite(fErEnqueue, buf, 12) ||
        !DmaWrite(fErEnqueue + 12, buf + 12, 4)) {
        fUsbSts |= USBSTS_HSE;
        return;
    }

    AdvanceEventEnqueue();
    if (!bei) {
        IntrRaise();
    }
}


void XHCIDevice::DrainEventFifo()
{
    while (fEventFifoCount > 0 && !EventRingFull()) {
        XHCIEvent ev = fEventFifo[0];
        memmove(fEventFifo, fEventFifo + 1,
                (fEventFifoCount - 1) * sizeof(XHCIEvent));
        fEventFifoCount--;
        PostEvent(ev, false);
    }
}


void XHCIDevice::PostCommandComplete(uint64_t cmd_addr, int cc, int slot_id)
{
    XHCIEvent ev;
    ev.parameter = cmd_addr;
    ev.status = (uint32_t)cc << 24;
    ev.control = (ER_COMMAND_COMPLETE << TRB_TYPE_SHIFT) |
                 ((uint32_t)slot_id << 24);
    PostEvent(ev, false);
}


//#pragma mark - rings

/* Fetch the next TRB, following Link TRBs. Returns the TRB type, or 0 when the
   ring is empty. Link TRBs are never part of a transfer descriptor and are
   never returned; the cycle state toggles only on one whose Toggle Cycle bit
   is set. */
int XHCIDevice::RingFetch(XHCIRing *ring, XHCITRB *trb)
{
    uint8_t buf[16];
    int hops = 0;

    for (;;) {
        if (!DmaRead(ring->deq, buf, 16)) {
            fUsbSts |= USBSTS_HSE;
            return 0;
        }
        trb->parameter = ((uint64_t)get_le32(buf + 4) << 32) | get_le32(buf);
        trb->status = get_le32(buf + 8);
        trb->control = get_le32(buf + 12);
        trb->addr = ring->deq;

        if (((trb->control & TRB_C) != 0) != ring->ccs) {
            return 0;
        }

        int type = TRB_TYPE(trb->control);
        if (type != TR_LINK) {
            ring->deq += 16;
            return type;
        }

        if (++hops > XHCI_MAX_LINK_HOPS) {
            vm_error("xhci: link TRB chain does not end\n");
            return 0;
        }
        ring->deq = trb->parameter & ~0xfULL;
        if ((trb->control & TRB_LK_TC) != 0) {
            ring->ccs = !ring->ccs;
        }
    }
}


//#pragma mark - contexts

uint64_t XHCIDevice::SlotCtxAddr(int slot_id)
{
    uint8_t buf[8];

    if (fDcbaap == 0 || !DmaRead(fDcbaap + (uint64_t)slot_id * 8, buf, 8)) {
        return 0;
    }
    return (((uint64_t)get_le32(buf + 4) << 32) | get_le32(buf)) & ~0x3fULL;
}


uint64_t XHCIDevice::EpCtxAddr(int slot_id, int dci)
{
    uint64_t base = SlotCtxAddr(slot_id);
    if (base == 0) {
        return 0;
    }
    return base + (uint64_t)dci * XHCI_CTX_SIZE;
}


uint32_t XHCIDevice::CtxRead(uint64_t base, int dword)
{
    uint8_t buf[4];

    if (base == 0 || !DmaRead(base + dword * 4, buf, 4)) {
        return 0;
    }
    return get_le32(buf);
}


void XHCIDevice::CtxWrite(uint64_t base, int dword, uint32_t val)
{
    uint8_t buf[4];

    if (base == 0) {
        return;
    }
    put_le32(buf, val);
    DmaWrite(base + dword * 4, buf, 4);
}


void XHCIDevice::SetSlotState(int slot_id, int state)
{
    uint64_t addr = SlotCtxAddr(slot_id);
    uint32_t dw3 = CtxRead(addr, 3);
    CtxWrite(addr, 3, (dw3 & 0x07ffffff) | ((uint32_t)state << 27));
}


void XHCIDevice::SetEpState(int slot_id, int dci, int state)
{
    uint64_t addr = EpCtxAddr(slot_id, dci);
    uint32_t dw0 = CtxRead(addr, 0);
    CtxWrite(addr, 0, (dw0 & ~0x7u) | (uint32_t)state);
}


//#pragma mark - endpoints

XHCIEndpoint *XHCIDevice::GetEndpoint(int slot_id, int dci)
{
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS || dci < 1 ||
        dci >= XHCI_DCI_COUNT) {
        return nullptr;
    }
    return fSlots[slot_id].ep[dci];
}


XHCIEndpoint *XHCIDevice::EnableEndpoint(int slot_id, int dci)
{
    XHCIEndpoint *ep = fSlots[slot_id].ep[dci];
    if (ep == nullptr) {
        ep = new XHCIEndpoint();
        fSlots[slot_id].ep[dci] = ep;
    }
    ep->xhci = this;
    ep->slot_id = slot_id;
    ep->dci = dci;
    ep->busy = false;
    return ep;
}


void XHCIDevice::DisableEndpoint(int slot_id, int dci)
{
    XHCIEndpoint *ep = fSlots[slot_id].ep[dci];
    if (ep == nullptr) {
        return;
    }
    if (ep->busy) {
        USBDevice *dev = fSlots[slot_id].dev;
        if (dev != nullptr) {
            dev->Cancel(&ep->xfer.urb);
        }
        ep->busy = false;
    }
    ep->state = EP_DISABLED;
}


/* Walk a route string down from a root hub port to the device it names. Each
   nibble is a downstream port number, and a zero nibble ends the walk. */
USBDevice *XHCIDevice::ResolveDevice(int port, uint32_t route)
{
    if (port < 1 || port > fPortCount) {
        return nullptr;
    }
    USBDevice *dev = fPorts[port - 1].port.dev;
    for (int tier = 0; tier < 5 && dev != nullptr; tier++) {
        int p = (route >> (tier * 4)) & 0xf;
        if (p == 0) {
            break;
        }
        dev = dev->DownstreamDevice(p);
    }
    return dev;
}


//#pragma mark - ports

void XHCIDevice::PortUpdate(int index)
{
    XHCIRootPort &p = fPorts[index];
    uint32_t portsc = PORTSC_PP; /* port power is fixed on: HCCPARAMS1.PPC=0 */
    uint32_t pls = PLS_RX_DETECT;

    if (p.port.dev != nullptr) {
        uint32_t speed;
        switch (p.port.dev->Speed()) {
        case USB_SPEED_LOW:   speed = PORT_SPEED_LOW; break;
        case USB_SPEED_FULL:  speed = PORT_SPEED_FULL; break;
        case USB_SPEED_SUPER: speed = PORT_SPEED_SUPER; break;
        default:              speed = PORT_SPEED_HIGH; break;
        }
        portsc |= PORTSC_CCS | (speed << PORTSC_SPEED_SHIFT);
        if (p.port.dev->Speed() == USB_SPEED_SUPER) {
            /* A SuperSpeed link trains itself, so the port comes up enabled. */
            portsc |= PORTSC_PED;
            pls = PLS_U0;
        } else {
            pls = PLS_POLLING;
        }
    }

    p.portsc = portsc | (pls << PORTSC_PLS_SHIFT);
    PortNotify(index, PORTSC_CSC);
}


void XHCIDevice::PortReset(int index, bool warm)
{
    XHCIRootPort &p = fPorts[index];

    if ((p.portsc & PORTSC_CCS) == 0) {
        /* Nothing attached: the reset bit simply reads back as zero and no
           change is reported. */
        return;
    }
    if (p.port.dev != nullptr) {
        p.port.dev->Reset();
    }
    if (warm && p.is_super) {
        p.portsc |= PORTSC_WRC;
    }
    p.portsc &= ~(PORTSC_PLS_MASK | PORTSC_PR);
    p.portsc |= (PLS_U0 << PORTSC_PLS_SHIFT) | PORTSC_PED;
    PortNotify(index, PORTSC_PRC);
}


void XHCIDevice::PostPortEvent(int index)
{
    XHCIEvent ev;
    ev.parameter = (uint64_t)(index + 1) << 24;
    ev.status = (uint32_t)CC_SUCCESS << 24;
    ev.control = ER_PORT_STATUS_CHANGE << TRB_TYPE_SHIFT;
    PostEvent(ev, false);
}


void XHCIDevice::PortNotify(int index, uint32_t bit)
{
    XHCIRootPort &p = fPorts[index];

    if ((p.portsc & bit) != 0) {
        /* Already pending: one event covers it. */
        return;
    }
    p.portsc |= bit;
    fUsbSts |= USBSTS_PCD;
    if ((fUsbCmd & USBCMD_RS) == 0) {
        /* Nowhere to send it yet. The change bit stays set, and the events
           are posted when the guest starts the controller. */
        return;
    }
    PostPortEvent(index);
}


int XHCIDevice::FindFreePort(USBSpeedEnum speed)
{
    int first = speed == USB_SPEED_SUPER ? 0 : fUsb3Ports;
    int last = speed == USB_SPEED_SUPER ? fUsb3Ports : fPortCount;

    for (int i = first; i < last; i++) {
        if (fPorts[i].port.dev == nullptr) {
            return i + 1;
        }
    }
    return 0;
}


bool XHCIDevice::AttachDevice(USBDevice *dev, int port)
{
    if (port < 1 || port > fPortCount) {
        vm_error("%s: port %d is out of range\n", Name(), port);
        return false;
    }
    XHCIRootPort &p = fPorts[port - 1];
    if (p.port.dev != nullptr) {
        vm_error("%s: port %d already has a device\n", Name(), port);
        return false;
    }
    if ((dev->Speed() == USB_SPEED_SUPER) != p.is_super) {
        vm_error("%s: port %d does not take a device of that speed\n", Name(),
                 port);
        return false;
    }

    p.port.dev = dev;
    dev->SetPort(&p.port);
    PortUpdate(port - 1);
    return true;
}


//#pragma mark - transfers

/* Collect one transfer descriptor from the endpoint's ring. Returns the number
   of TRBs, 0 when the ring is empty or the descriptor is not fully written
   yet, and -1 when it is malformed. */
int XHCIDevice::FetchTD(XHCIEndpoint *ep)
{
    XHCITransfer *x = &ep->xfer;
    XHCIRing probe = ep->ring;
    XHCITRB trb;
    bool control_td_set = false;
    int count = 0;

    /* First pass: count the TRBs without moving the endpoint's own dequeue
       pointer, so that a descriptor the guest has only half written is left
       alone rather than started. */
    for (;;) {
        int type = RingFetch(&probe, &trb);
        if (type == 0) {
            return 0;
        }
        count++;
        if (count > XHCI_MAX_TRBS_PER_TD) {
            vm_error("xhci: transfer descriptor longer than %d TRBs\n",
                     XHCI_MAX_TRBS_PER_TD);
            return -1;
        }
        /* A control transfer arrives as separate Setup, Data and Status
           descriptors, none of them chained, but it is one transfer; keep
           collecting until the Status stage closes it. */
        if (type == TR_SETUP) {
            control_td_set = true;
        } else if (type == TR_STATUS) {
            control_td_set = false;
        }
        if (!control_td_set && (trb.control & TRB_TR_CH) == 0) {
            break;
        }
    }

    x->snap_deq = ep->ring.deq;
    x->snap_ccs = ep->ring.ccs;
    x->trb_count = 0;
    for (int i = 0; i < count; i++) {
        if (RingFetch(&ep->ring, &x->trbs[x->trb_count]) == 0) {
            return -1;
        }
        x->trb_count++;
    }
    return count;
}


/* Post the Transfer Events one finished descriptor calls for. An event is
   produced only for a TRB that asked for one -- Interrupt On Completion, or
   Interrupt on Short Packet when the transfer came up short -- because a host
   that receives an event it did not ask for treats it as a controller fault.
   The length reported is the residual, except on an Event Data TRB where it is
   the running total of everything transferred since the last one. */
void XHCIDevice::XferReport(XHCIEndpoint *ep)
{
    XHCITransfer *x = &ep->xfer;
    uint32_t edtla = 0;
    uint32_t left = x->actual_length;
    bool reported = false;
    bool shortpkt = false;

    for (int i = 0; i < x->trb_count; i++) {
        const XHCITRB &trb = x->trbs[i];
        int type = TRB_TYPE(trb.control);
        uint32_t chunk = 0;

        switch (type) {
        case TR_SETUP:
            chunk = trb.status & TRB_LEN_MASK;
            if (chunk > 8) {
                chunk = 8;
            }
            break;
        case TR_DATA:
        case TR_NORMAL:
        case TR_ISOCH:
            chunk = trb.status & TRB_LEN_MASK;
            if (chunk > left) {
                chunk = left;
                if (x->cc == CC_SUCCESS) {
                    shortpkt = true;
                }
            }
            left -= chunk;
            edtla += chunk;
            break;
        case TR_STATUS:
            reported = false;
            shortpkt = false;
            break;
        }

        if (!reported &&
            (((trb.control & TRB_TR_IOC) != 0) ||
             (shortpkt && (trb.control & TRB_TR_ISP) != 0) ||
             (x->cc != CC_SUCCESS && left == 0))) {
            XHCIEvent ev;
            ev.parameter = trb.addr;
            uint32_t length = (trb.status & TRB_LEN_MASK) - chunk;
            uint32_t control = (ER_TRANSFER << TRB_TYPE_SHIFT) |
                               ((uint32_t)ep->dci << 16) |
                               ((uint32_t)ep->slot_id << 24);
            uint32_t cc = x->cc == CC_SUCCESS
                              ? (shortpkt ? CC_SHORT_PACKET : CC_SUCCESS)
                              : x->cc;
            if (type == TR_EVDATA) {
                /* The guest gets its own cookie back rather than a TRB
                   address, and the length is cumulative. */
                ev.parameter = trb.parameter;
                control |= TRB_EV_ED;
                length = edtla & 0xffffff;
                edtla = 0;
            }
            ev.status = (length & 0xffffff) | (cc << 24);
            ev.control = control;
            PostEvent(ev, (trb.control & TRB_TR_BEI) != 0);
            reported = true;
            if (x->cc != CC_SUCCESS) {
                return;
            }
        }

        if (type == TR_SETUP) {
            reported = false;
            shortpkt = false;
        }
    }
}


void XHCIDevice::HaltEndpoint(XHCIEndpoint *ep)
{
    ep->state = EP_HALTED;
    SetEpState(ep->slot_id, ep->dci, EP_HALTED);
    uint64_t addr = EpCtxAddr(ep->slot_id, ep->dci);
    CtxWrite(addr, 2, (uint32_t)(ep->ring.deq & 0xfffffff0) |
                          (ep->ring.ccs ? 1 : 0));
    CtxWrite(addr, 3, (uint32_t)(ep->ring.deq >> 32));
}


static uint32_t xhci_map_status(USBStatusEnum status)
{
    switch (status) {
    case USB_STATUS_OK:      return CC_SUCCESS;
    case USB_STATUS_STALL:   return CC_STALL;
    case USB_STATUS_BABBLE:  return CC_BABBLE;
    case USB_STATUS_NODEV:   return CC_USB_TRANSACTION_ERROR;
    case USB_STATUS_IOERROR: return CC_DATA_BUFFER_ERROR;
    default:                 return CC_USB_TRANSACTION_ERROR;
    }
}


void XHCIDevice::KickEndpoint(XHCIEndpoint *ep)
{
    if (ep == nullptr || ep->state != EP_RUNNING || ep->busy) {
        return;
    }
    if (ep->kicking) {
        return;
    }
    ep->kicking = true;

    USBDevice *dev = fSlots[ep->slot_id].dev;

    while (ep->state == EP_RUNNING && !ep->busy) {
        int count = FetchTD(ep);
        if (count <= 0) {
            break;
        }

        XHCITransfer *x = &ep->xfer;
        URB *urb = &x->urb;
        *urb = URB();
        x->cc = CC_SUCCESS;
        x->actual_length = 0;
        x->buffer.Init(this, x->trbs, x->trb_count);

        urb->dev = dev;
        urb->buffer = &x->buffer;
        urb->completion = ep;

        if (ep->dci == 1) {
            /* Control: the setup packet is the first TRB's parameter field,
               and the direction the request itself declares is the one that
               counts. */
            if (TRB_TYPE(x->trbs[0].control) != TR_SETUP) {
                x->cc = CC_TRB_ERROR;
                XferReport(ep);
                continue;
            }
            uint8_t setup[8];
            put_le64(setup, x->trbs[0].parameter);
            urb->type = USB_ENDPOINT_CONTROL;
            urb->endpoint = 0;
            urb->setup.request_type = setup[0];
            urb->setup.request = setup[1];
            urb->setup.value = get_le16(setup + 2);
            urb->setup.index = get_le16(setup + 4);
            urb->setup.length = get_le16(setup + 6);
            urb->is_in = (setup[0] & USB_DIR_IN) != 0;
        } else {
            urb->endpoint = ep->dci >> 1;
            urb->is_in = (ep->dci & 1) != 0;
            switch (ep->type) {
            case EP_TYPE_ISOCH_IN:
            case EP_TYPE_ISOCH_OUT:
                urb->type = USB_ENDPOINT_ISOCH;
                break;
            case EP_TYPE_INTR_IN:
            case EP_TYPE_INTR_OUT:
                urb->type = USB_ENDPOINT_INTERRUPT;
                break;
            default:
                urb->type = USB_ENDPOINT_BULK;
                break;
            }
        }

        if (dev == nullptr) {
            x->cc = CC_USB_TRANSACTION_ERROR;
            XferReport(ep);
            continue;
        }

        USBStatusEnum status = dev->Submit(urb);
        if (status == USB_STATUS_ASYNC) {
            /* The endpoint keeps the descriptor and stops fetching; the
               device's completion picks it up again. */
            ep->busy = true;
            break;
        }

        x->cc = xhci_map_status(status);
        x->actual_length = urb->actual_length;
        XferReport(ep);
        if (x->cc == CC_STALL) {
            HaltEndpoint(ep);
        }
    }

    ep->kicking = false;
}


void XHCIDevice::EndpointComplete(XHCIEndpoint *ep, URB *urb)
{
    XHCITransfer *x = &ep->xfer;

    if (!ep->busy) {
        return;
    }
    ep->busy = false;
    x->cc = xhci_map_status(urb->status);
    x->actual_length = urb->actual_length;
    XferReport(ep);
    if (x->cc == CC_STALL) {
        HaltEndpoint(ep);
    }
    /* Whatever the guest queued while the device had the descriptor. */
    KickEndpoint(ep);
}


void XHCIEndpoint::Complete(URB *urb)
{
    xhci->EndpointComplete(this, urb);
}


//#pragma mark - commands

int XHCIDevice::CmdEnableSlot(int *pslot_id)
{
    int max = fConfig & 0xff;
    if (max == 0 || max > XHCI_MAX_SLOTS) {
        max = XHCI_MAX_SLOTS;
    }
    for (int i = 1; i <= max; i++) {
        if (!fSlots[i].enabled) {
            /* The endpoint objects are kept and reused; only the state they
               carry is cleared, so that enabling a slot allocates nothing. */
            for (int dci = 1; dci < XHCI_DCI_COUNT; dci++) {
                if (fSlots[i].ep[dci] != nullptr) {
                    DisableEndpoint(i, dci);
                }
            }
            fSlots[i].enabled = true;
            fSlots[i].dev = nullptr;
            fSlots[i].port = 0;
            *pslot_id = i;
            return CC_SUCCESS;
        }
    }
    *pslot_id = 0;
    return CC_NO_SLOTS;
}


int XHCIDevice::CmdDisableSlot(int slot_id)
{
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS || !fSlots[slot_id].enabled) {
        return CC_SLOT_NOT_ENABLED;
    }
    for (int dci = 1; dci < XHCI_DCI_COUNT; dci++) {
        if (fSlots[slot_id].ep[dci] != nullptr) {
            DisableEndpoint(slot_id, dci);
            SetEpState(slot_id, dci, EP_DISABLED);
        }
    }
    SetSlotState(slot_id, SLOT_DISABLED);
    fSlots[slot_id].enabled = false;
    fSlots[slot_id].dev = nullptr;
    return CC_SUCCESS;
}


/* Copy one 32 byte context from the input context to the output context. */
static void xhci_copy_ctx(XHCIDevice *xhci, uint64_t dst, uint64_t src)
{
    uint8_t buf[XHCI_CTX_SIZE];

    if (xhci->DmaRead(src, buf, sizeof(buf))) {
        xhci->DmaWrite(dst, buf, sizeof(buf));
    }
}


int XHCIDevice::CmdAddressDevice(int slot_id, const XHCITRB &trb)
{
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS || !fSlots[slot_id].enabled) {
        return CC_SLOT_NOT_ENABLED;
    }

    uint64_t in_ctx = trb.parameter & ~0xfULL;
    uint32_t add_flags = CtxRead(in_ctx, 1);
    if ((add_flags & 0x3) != 0x3) {
        /* The slot context and the default control endpoint are the two the
           command exists to install. */
        return CC_PARAMETER_ERROR;
    }

    uint64_t in_slot = in_ctx + XHCI_CTX_SIZE;
    uint64_t in_ep0 = in_ctx + 2 * XHCI_CTX_SIZE;
    uint64_t out_slot = SlotCtxAddr(slot_id);
    if (out_slot == 0) {
        return CC_PARAMETER_ERROR;
    }

    uint32_t slot_dw0 = CtxRead(in_slot, 0);
    uint32_t slot_dw1 = CtxRead(in_slot, 1);
    uint32_t route = slot_dw0 & 0xfffff;
    int port = (slot_dw1 >> 16) & 0xff;

    USBDevice *dev = ResolveDevice(port, route);
    if (dev == nullptr) {
        return CC_USB_TRANSACTION_ERROR;
    }

    xhci_copy_ctx(this, out_slot, in_slot);
    xhci_copy_ctx(this, out_slot + XHCI_CTX_SIZE, in_ep0);

    fSlots[slot_id].dev = dev;
    fSlots[slot_id].port = port;

    XHCIEndpoint *ep = EnableEndpoint(slot_id, 1);
    ep->type = EP_TYPE_CONTROL;
    ep->max_packet = CtxRead(in_ep0, 1) >> 16;
    uint32_t deq_lo = CtxRead(in_ep0, 2);
    ep->ring.deq = (((uint64_t)CtxRead(in_ep0, 3) << 32) | deq_lo) & ~0xfULL;
    ep->ring.ccs = (deq_lo & 1) != 0;
    ep->state = EP_RUNNING;

    int address = 0;
    int slot_state = SLOT_DEFAULT;
    if ((trb.control & TRB_CR_BSR) == 0) {
        /* Without Block Set Address the command really does address the
           device. The slot id is unique, so it serves as the address. */
        URB urb {};
        urb.dev = dev;
        urb.type = USB_ENDPOINT_CONTROL;
        urb.setup.request_type = 0x00;
        urb.setup.request = USB_REQ_SET_ADDRESS;
        urb.setup.value = slot_id;
        if (dev->Submit(&urb) != USB_STATUS_OK) {
            return CC_USB_TRANSACTION_ERROR;
        }
        address = slot_id;
        slot_state = SLOT_ADDRESSED;
    }

    uint32_t out_dw3 = CtxRead(out_slot, 3);
    CtxWrite(out_slot, 3,
             (out_dw3 & 0x07ffff00) | (uint32_t)address |
                 ((uint32_t)slot_state << 27));
    uint32_t out_dw0 = CtxRead(out_slot, 0);
    CtxWrite(out_slot, 0, (out_dw0 & 0x07ffffff) | (1u << 27));
    SetEpState(slot_id, 1, EP_RUNNING);
    return CC_SUCCESS;
}


int XHCIDevice::CmdConfigureEndpoint(int slot_id, const XHCITRB &trb)
{
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS || !fSlots[slot_id].enabled) {
        return CC_SLOT_NOT_ENABLED;
    }

    uint64_t out_slot = SlotCtxAddr(slot_id);
    int slot_state = CtxRead(out_slot, 3) >> 27;
    if (slot_state != SLOT_ADDRESSED && slot_state != SLOT_CONFIGURED) {
        return CC_CONTEXT_STATE_ERROR;
    }

    if ((trb.control & TRB_CR_DC) != 0) {
        /* Deconfigure: the input context is not consulted at all. */
        for (int dci = 2; dci < XHCI_DCI_COUNT; dci++) {
            DisableEndpoint(slot_id, dci);
            SetEpState(slot_id, dci, EP_DISABLED);
        }
        uint32_t dw0 = CtxRead(out_slot, 0);
        CtxWrite(out_slot, 0, (dw0 & 0x07ffffff) | (1u << 27));
        SetSlotState(slot_id, SLOT_ADDRESSED);
        return CC_SUCCESS;
    }

    uint64_t in_ctx = trb.parameter & ~0xfULL;
    uint32_t drop_flags = CtxRead(in_ctx, 0);
    uint32_t add_flags = CtxRead(in_ctx, 1);

    /* The first two flags address the slot context and the default control
       endpoint, neither of which this command is allowed to drop. */
    if ((drop_flags & 0x3) != 0) {
        return CC_PARAMETER_ERROR;
    }

    for (int dci = 2; dci < XHCI_DCI_COUNT; dci++) {
        if ((drop_flags & (1u << dci)) != 0) {
            DisableEndpoint(slot_id, dci);
            SetEpState(slot_id, dci, EP_DISABLED);
        }
    }

    for (int dci = 1; dci < XHCI_DCI_COUNT; dci++) {
        if ((add_flags & (1u << dci)) == 0) {
            continue;
        }
        uint64_t in_ep = in_ctx + (uint64_t)(dci + 1) * XHCI_CTX_SIZE;
        uint64_t out_ep = EpCtxAddr(slot_id, dci);
        xhci_copy_ctx(this, out_ep, in_ep);

        XHCIEndpoint *ep = EnableEndpoint(slot_id, dci);
        uint32_t dw1 = CtxRead(in_ep, 1);
        ep->type = (dw1 >> 3) & 0x7;
        ep->max_packet = dw1 >> 16;
        uint32_t deq_lo = CtxRead(in_ep, 2);
        ep->ring.deq =
            (((uint64_t)CtxRead(in_ep, 3) << 32) | deq_lo) & ~0xfULL;
        ep->ring.ccs = (deq_lo & 1) != 0;
        ep->state = EP_RUNNING;
        SetEpState(slot_id, dci, EP_RUNNING);
    }

    if ((add_flags & 0x1) != 0) {
        /* The slot context carries the hub flag and the transaction
           translator fields, which a host sets when it enumerates a hub. */
        uint64_t in_slot = in_ctx + XHCI_CTX_SIZE;
        uint32_t in_dw0 = CtxRead(in_slot, 0);
        uint32_t in_dw1 = CtxRead(in_slot, 1);
        uint32_t in_dw2 = CtxRead(in_slot, 2);
        uint32_t out_dw0 = CtxRead(out_slot, 0);
        /* Keep the route string; take the speed, the hub flag and the
           context entry count from the input context. */
        CtxWrite(out_slot, 0,
                 (out_dw0 & 0x000fffff) | (in_dw0 & 0xfff00000));
        CtxWrite(out_slot, 1, in_dw1);
        CtxWrite(out_slot, 2, in_dw2);
    }

    SetSlotState(slot_id, SLOT_CONFIGURED);
    return CC_SUCCESS;
}


int XHCIDevice::CmdEvaluateContext(int slot_id, const XHCITRB &trb)
{
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS || !fSlots[slot_id].enabled) {
        return CC_SLOT_NOT_ENABLED;
    }

    uint64_t in_ctx = trb.parameter & ~0xfULL;
    uint32_t add_flags = CtxRead(in_ctx, 1);
    uint64_t out_slot = SlotCtxAddr(slot_id);

    /* Only two things are evaluated: the interrupter and maximum exit latency
       in the slot context, and the maximum packet size on the default control
       endpoint. A host uses the latter once it has read the real descriptor
       and found the value it guessed was wrong. */
    if ((add_flags & 0x1) != 0) {
        uint64_t in_slot = in_ctx + XHCI_CTX_SIZE;
        uint32_t in_dw1 = CtxRead(in_slot, 1);
        uint32_t in_dw2 = CtxRead(in_slot, 2);
        uint32_t out_dw1 = CtxRead(out_slot, 1);
        uint32_t out_dw2 = CtxRead(out_slot, 2);
        CtxWrite(out_slot, 1, (out_dw1 & 0xffff0000) | (in_dw1 & 0xffff));
        CtxWrite(out_slot, 2, (out_dw2 & 0x003fffff) | (in_dw2 & 0xffc00000));
    }
    if ((add_flags & 0x2) != 0) {
        uint64_t in_ep0 = in_ctx + 2 * XHCI_CTX_SIZE;
        uint64_t out_ep0 = EpCtxAddr(slot_id, 1);
        uint32_t in_dw1 = CtxRead(in_ep0, 1);
        uint32_t out_dw1 = CtxRead(out_ep0, 1);
        CtxWrite(out_ep0, 1, (out_dw1 & 0xffff) | (in_dw1 & 0xffff0000));
        XHCIEndpoint *ep = GetEndpoint(slot_id, 1);
        if (ep != nullptr) {
            ep->max_packet = in_dw1 >> 16;
        }
    }
    return CC_SUCCESS;
}


int XHCIDevice::CmdResetEndpoint(int slot_id, int dci)
{
    XHCIEndpoint *ep = GetEndpoint(slot_id, dci);
    if (ep == nullptr || ep->state == EP_DISABLED) {
        return CC_CONTEXT_STATE_ERROR;
    }
    /* A host can reach here with an endpoint that is not halted, and refusing
       the command there only confuses its recovery. */
    ep->state = EP_STOPPED;
    SetEpState(slot_id, dci, EP_STOPPED);
    return CC_SUCCESS;
}


int XHCIDevice::CmdStopEndpoint(int slot_id, int dci)
{
    XHCIEndpoint *ep = GetEndpoint(slot_id, dci);
    if (ep == nullptr || ep->state == EP_DISABLED) {
        return CC_CONTEXT_STATE_ERROR;
    }

    if (ep->busy) {
        USBDevice *dev = fSlots[slot_id].dev;
        if (dev != nullptr) {
            dev->Cancel(&ep->xfer.urb);
        }
        ep->busy = false;
        /* The descriptor's own event comes before the command's, and the ring
           rewinds to its start so the guest can retry it. */
        ep->xfer.cc = ep->xfer.actual_length == 0 ? CC_STOPPED_LENGTH_INVALID
                                                  : CC_STOPPED;
        XferReport(ep);
        ep->ring.deq = ep->xfer.snap_deq;
        ep->ring.ccs = ep->xfer.snap_ccs;
    }

    ep->state = EP_STOPPED;
    SetEpState(slot_id, dci, EP_STOPPED);
    uint64_t addr = EpCtxAddr(slot_id, dci);
    CtxWrite(addr, 2,
             (uint32_t)(ep->ring.deq & 0xfffffff0) | (ep->ring.ccs ? 1 : 0));
    CtxWrite(addr, 3, (uint32_t)(ep->ring.deq >> 32));
    return CC_SUCCESS;
}


int XHCIDevice::CmdSetTrDequeue(int slot_id, int dci, const XHCITRB &trb)
{
    XHCIEndpoint *ep = GetEndpoint(slot_id, dci);
    if (ep == nullptr || ep->state == EP_DISABLED) {
        return CC_CONTEXT_STATE_ERROR;
    }
    if (ep->state == EP_RUNNING || ep->busy) {
        return CC_CONTEXT_STATE_ERROR;
    }

    ep->ring.deq = trb.parameter & ~0xfULL;
    ep->ring.ccs = (trb.parameter & 1) != 0;
    uint64_t addr = EpCtxAddr(slot_id, dci);
    CtxWrite(addr, 2,
             (uint32_t)(ep->ring.deq & 0xfffffff0) | (ep->ring.ccs ? 1 : 0));
    CtxWrite(addr, 3, (uint32_t)(ep->ring.deq >> 32));
    return CC_SUCCESS;
}


int XHCIDevice::CmdResetDevice(int slot_id)
{
    if (slot_id < 1 || slot_id > XHCI_MAX_SLOTS || !fSlots[slot_id].enabled) {
        return CC_SLOT_NOT_ENABLED;
    }
    uint64_t out_slot = SlotCtxAddr(slot_id);
    int slot_state = CtxRead(out_slot, 3) >> 27;
    if (slot_state != SLOT_ADDRESSED && slot_state != SLOT_CONFIGURED) {
        return CC_CONTEXT_STATE_ERROR;
    }

    for (int dci = 2; dci < XHCI_DCI_COUNT; dci++) {
        DisableEndpoint(slot_id, dci);
        SetEpState(slot_id, dci, EP_DISABLED);
    }
    uint32_t dw3 = CtxRead(out_slot, 3);
    CtxWrite(out_slot, 3,
             (dw3 & 0x07ffff00) | ((uint32_t)SLOT_DEFAULT << 27));
    uint32_t dw0 = CtxRead(out_slot, 0);
    CtxWrite(out_slot, 0, (dw0 & 0x07ffffff) | (1u << 27));
    return CC_SUCCESS;
}


void XHCIDevice::ExecuteCommand(uint64_t addr, const XHCITRB &trb)
{
    int type = TRB_TYPE(trb.control);
    int slot_id = (trb.control >> 24) & 0xff;
    int dci = (trb.control >> 16) & 0x1f;
    int cc;

    switch (type) {
    case CR_NOOP:
        PostCommandComplete(addr, CC_SUCCESS, 0);
        return;

    case CR_ENABLE_SLOT: {
        int new_slot = 0;
        cc = CmdEnableSlot(&new_slot);
        PostCommandComplete(addr, cc, new_slot);
        return;
    }

    case CR_DISABLE_SLOT:
        cc = CmdDisableSlot(slot_id);
        break;

    case CR_ADDRESS_DEVICE:
        cc = CmdAddressDevice(slot_id, trb);
        break;

    case CR_CONFIGURE_ENDPOINT:
        cc = CmdConfigureEndpoint(slot_id, trb);
        break;

    case CR_EVALUATE_CONTEXT:
        cc = CmdEvaluateContext(slot_id, trb);
        break;

    case CR_RESET_ENDPOINT:
        cc = CmdResetEndpoint(slot_id, dci);
        break;

    case CR_STOP_ENDPOINT:
        cc = CmdStopEndpoint(slot_id, dci);
        break;

    case CR_SET_TR_DEQUEUE:
        cc = CmdSetTrDequeue(slot_id, dci, trb);
        break;

    case CR_RESET_DEVICE:
        cc = CmdResetDevice(slot_id);
        break;

    default:
        /* A command left unanswered is what a host reads as a dead
           controller, so every one of them gets a completion event. */
        xhci_debug("unsupported command %d\n", type);
        cc = CC_TRB_ERROR;
        break;
    }

    PostCommandComplete(addr, cc, slot_id);
}


void XHCIDevice::RunCommandRing()
{
    XHCITRB trb;

    if ((fUsbCmd & USBCMD_RS) == 0 || fInCmdRing) {
        return;
    }
    fInCmdRing = true;
    fCrcrLo |= CRCR_CRR;

    while (RingFetch(&fCmdRing, &trb) != 0) {
        ExecuteCommand(trb.addr, trb);
        if ((fCrcrLo & CRCR_CRR) == 0) {
            /* A stop or an abort landed while the ring was running. */
            break;
        }
    }

    fCrcrLo &= ~CRCR_CRR;
    fInCmdRing = false;
}


//#pragma mark - controller

uint32_t XHCIDevice::MfIndex() const
{
    uint64_t us = host_monotonic_us();
    return (uint32_t)(((us - fStartUs) / 125) & 0x3fff);
}


void XHCIDevice::Reset()
{
    for (int i = 1; i <= XHCI_MAX_SLOTS; i++) {
        for (int dci = 1; dci < XHCI_DCI_COUNT; dci++) {
            if (fSlots[i].ep[dci] != nullptr) {
                DisableEndpoint(i, dci);
            }
        }
        fSlots[i].enabled = false;
        fSlots[i].dev = nullptr;
    }

    fUsbCmd = 0;
    fUsbSts = USBSTS_HCH;
    fDnCtrl = 0;
    fConfig = 0;
    fCrcrLo = 0;
    fCrcrHi = 0;
    fCmdRing = XHCIRing();
    fDcbaap = 0;
    fDcbaapLo = 0;

    fIman = 0;
    fImod = 0;
    fErstSz = 0;
    fErstBa = 0;
    fErstBaLo = 0;
    fErdpLo = 0;
    fErdpHi = 0;
    fErSegStart = 0;
    fErSegSize = 0;
    fErSegIndex = 0;
    fErSegOffset = 0;
    fErPcs = true;
    fEventFifoCount = 0;
    fMsixSent = false;

    /* A host controller reset resets the ports too, which leaves an attached
       device reported as newly connected. */
    for (int i = 0; i < fPortCount; i++) {
        PortUpdate(i);
    }
    IntrUpdate();
}


void XHCIDevice::SetRunning(bool running)
{
    if (running) {
        fUsbSts &= ~USBSTS_HCH;
        /* Every device here was plugged in before the guest booted, so their
           connect changes are already pending and have to be announced now
           that there is somewhere to send them. */
        for (int i = 0; i < fPortCount; i++) {
            if ((fPorts[i].portsc & PORTSC_CHANGE_MASK) != 0) {
                PostPortEvent(i);
            }
        }
    } else {
        for (int i = 1; i <= XHCI_MAX_SLOTS; i++) {
            for (int dci = 1; dci < XHCI_DCI_COUNT; dci++) {
                XHCIEndpoint *ep = fSlots[i].ep[dci];
                if (ep != nullptr && ep->busy) {
                    if (fSlots[i].dev != nullptr) {
                        fSlots[i].dev->Cancel(&ep->xfer.urb);
                    }
                    ep->busy = false;
                }
            }
        }
        fUsbSts |= USBSTS_HCH;
    }
}


//#pragma mark - register file

uint32_t XHCIDevice::CapRead(uint32_t offset)
{
    switch (offset) {
    case XHCI_CAPLENGTH:
        return XHCI_CAP_LENGTH | (XHCI_HCIVERSION << 16);

    case XHCI_HCSPARAMS1:
        return XHCI_MAX_SLOTS | (1 << 8) | ((uint32_t)fPortCount << 24);

    case XHCI_HCSPARAMS2:
        /* Isochronous scheduling threshold of seven frames, one event ring
           segment, and no scratchpad buffers: a controller that asked for
           scratchpad space would oblige the guest to interpret entry zero of
           the device context base address array. */
        return 0x0000000f;

    case XHCI_HCSPARAMS3:
        return 0;

    case XHCI_HCCPARAMS1:
        /* 32-bit addressing, 32-byte contexts, no streams, and the extended
           capability list eight dwords in. */
        return (XHCI_EXTCAP_OFFSET / 4) << 16;

    case XHCI_DBOFF:
        return XHCI_DOORBELL_OFFSET;

    case XHCI_RTSOFF:
        return XHCI_RUNTIME_OFFSET;

    case XHCI_HCCPARAMS2:
        return 0;
    }

    if (offset >= XHCI_EXTCAP_OFFSET && offset < XHCI_OPER_OFFSET) {
        return ExtCapRead(offset - XHCI_EXTCAP_OFFSET);
    }
    return 0;
}


/* The Supported Protocol capabilities. Without them a host has no idea which
   port speaks which revision of USB, and Linux refuses the controller
   outright. The "next" field counts dwords. */
uint32_t XHCIDevice::ExtCapRead(uint32_t offset)
{
    switch (offset) {
    /* USB 2.00, covering the high speed ports, which follow the SuperSpeed
       ones. */
    case 0x00:
        return 0x02000002 | (4 << 8);
    case 0x04:
        return 0x20425355; /* "USB " */
    case 0x08:
        return (uint32_t)(fUsb3Ports + 1) | ((uint32_t)fUsb2Ports << 8);
    case 0x0c:
        return 0;

    /* USB 3.00, covering the SuperSpeed ports, and ending the list. */
    case 0x10:
        return 0x03000002;
    case 0x14:
        return 0x20425355;
    case 0x18:
        return 1 | ((uint32_t)fUsb3Ports << 8);
    case 0x1c:
        return 0;
    }
    return 0;
}


uint32_t XHCIDevice::OperRead(uint32_t offset)
{
    switch (offset) {
    case XHCI_USBCMD:
        return fUsbCmd;
    case XHCI_USBSTS:
        return fUsbSts;
    case XHCI_PAGESIZE:
        return 1; /* 4 KB pages */
    case XHCI_DNCTRL:
        return fDnCtrl;
    case XHCI_CRCR_LO:
        /* The command ring's own status bits always read back as zero: the
           ring only ever runs inside the write that rang its doorbell. */
        return fCrcrLo & ~(CRCR_CS | CRCR_CA | CRCR_CRR);
    case XHCI_CRCR_HI:
        return fCrcrHi;
    case XHCI_DCBAAP_LO:
        return (uint32_t)fDcbaap;
    case XHCI_DCBAAP_HI:
        return (uint32_t)(fDcbaap >> 32);
    case XHCI_CONFIG:
        return fConfig;
    }
    return 0;
}


void XHCIDevice::OperWrite(uint32_t offset, uint32_t val)
{
    switch (offset) {
    case XHCI_USBCMD: {
        if ((val & USBCMD_HCRST) != 0) {
            /* The reset completes inside this write, so the bit reads back
               as zero and the controller-not-ready bit never sets. */
            Reset();
            return;
        }
        bool was_running = (fUsbCmd & USBCMD_RS) != 0;
        /* The save and restore bits are accepted and self-clear; there is no
           state here worth saving. */
        fUsbCmd = val & (USBCMD_RS | USBCMD_INTE | USBCMD_HSEE | USBCMD_EWE |
                         USBCMD_EU3S | USBCMD_CME);
        bool running = (fUsbCmd & USBCMD_RS) != 0;
        if (running != was_running) {
            SetRunning(running);
        }
        IntrUpdate();
        return;
    }

    case XHCI_USBSTS:
        /* Write one to clear, on the bits that allow it. */
        fUsbSts &= ~(val & (USBSTS_HSE | USBSTS_EINT | USBSTS_PCD));
        IntrUpdate();
        return;

    case XHCI_DNCTRL:
        fDnCtrl = val & 0xffff;
        return;

    case XHCI_CRCR_LO:
        /* Latched only; the high half is what commits. */
        fCrcrLo = (val & 0xffffffcf) | (fCrcrLo & CRCR_CRR);
        return;

    case XHCI_CRCR_HI:
        fCrcrHi = val;
        if ((fCrcrLo & (CRCR_CS | CRCR_CA)) != 0 &&
            (fCrcrLo & CRCR_CRR) != 0) {
            fCrcrLo &= ~CRCR_CRR;
            PostCommandComplete(fCmdRing.deq, CC_COMMAND_RING_STOPPED, 0);
        } else {
            fCmdRing.deq =
                (((uint64_t)fCrcrHi << 32) | (fCrcrLo & ~0x3fu));
            fCmdRing.ccs = (fCrcrLo & CRCR_RCS) != 0;
        }
        return;

    case XHCI_DCBAAP_LO:
        fDcbaapLo = val;
        return;

    case XHCI_DCBAAP_HI:
        fDcbaap = (((uint64_t)val << 32) | fDcbaapLo) & ~0x3fULL;
        return;

    case XHCI_CONFIG:
        fConfig = val & 0xff;
        return;
    }
}


uint32_t XHCIDevice::PortRead(uint32_t offset)
{
    int index = offset / XHCI_PORT_STRIDE;
    int reg = offset % XHCI_PORT_STRIDE;

    if (index >= fPortCount) {
        return 0;
    }
    if (reg == 0) {
        return fPorts[index].portsc;
    }
    /* Power management, link info and hardware LPM have nothing to report on
       a controller with no real link behind it. */
    return 0;
}


void XHCIDevice::PortWrite(uint32_t offset, uint32_t val)
{
    int index = offset / XHCI_PORT_STRIDE;
    int reg = offset % XHCI_PORT_STRIDE;

    if (index >= fPortCount || reg != 0) {
        return;
    }

    if ((val & PORTSC_WPR) != 0) {
        PortReset(index, true);
        return;
    }
    if ((val & PORTSC_PR) != 0) {
        PortReset(index, false);
        return;
    }

    uint32_t portsc = fPorts[index].portsc;
    uint32_t notify = 0;

    /* The change bits are write one to clear. */
    portsc &= ~(val & PORTSC_CHANGE_MASK);

    if ((val & PORTSC_LWS) != 0) {
        uint32_t old_pls = (fPorts[index].portsc & PORTSC_PLS_MASK)
                           >> PORTSC_PLS_SHIFT;
        uint32_t new_pls = (val & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (new_pls == PLS_U0 && old_pls != PLS_U0) {
            portsc = (portsc & ~PORTSC_PLS_MASK) | (PLS_U0 << PORTSC_PLS_SHIFT);
            notify = PORTSC_PLC;
        } else if (new_pls == PLS_U3 && old_pls < PLS_U3) {
            portsc = (portsc & ~PORTSC_PLS_MASK) | (PLS_U3 << PORTSC_PLS_SHIFT);
        }
    }

    portsc &= ~(PORTSC_PP | PORTSC_WCE | PORTSC_WDE | PORTSC_WOE);
    portsc |= val & (PORTSC_PP | PORTSC_WCE | PORTSC_WDE | PORTSC_WOE);
    fPorts[index].portsc = portsc;

    if (notify != 0) {
        PortNotify(index, notify);
    }
}


uint32_t XHCIDevice::RuntimeRead(uint32_t offset)
{
    if (offset == 0) {
        return MfIndex();
    }
    if (offset < XHCI_INTR_OFFSET - XHCI_RUNTIME_OFFSET ||
        offset >= XHCI_INTR_OFFSET - XHCI_RUNTIME_OFFSET + 0x20) {
        /* Only one interrupter exists, as HCSPARAMS1 says. */
        return 0;
    }

    switch (offset - (XHCI_INTR_OFFSET - XHCI_RUNTIME_OFFSET)) {
    case XHCI_IMAN:
        return fIman;
    case XHCI_IMOD:
        return fImod;
    case XHCI_ERSTSZ:
        return fErstSz;
    case XHCI_ERSTBA_LO:
        return (uint32_t)fErstBa;
    case XHCI_ERSTBA_HI:
        return (uint32_t)(fErstBa >> 32);
    case XHCI_ERDP_LO:
        return fErdpLo;
    case XHCI_ERDP_HI:
        return fErdpHi;
    }
    return 0;
}


void XHCIDevice::RuntimeWrite(uint32_t offset, uint32_t val)
{
    if (offset < XHCI_INTR_OFFSET - XHCI_RUNTIME_OFFSET ||
        offset >= XHCI_INTR_OFFSET - XHCI_RUNTIME_OFFSET + 0x20) {
        return;
    }

    switch (offset - (XHCI_INTR_OFFSET - XHCI_RUNTIME_OFFSET)) {
    case XHCI_IMAN:
        if ((val & IMAN_IP) != 0) {
            fIman &= ~IMAN_IP;
        }
        fIman = (fIman & IMAN_IP) | (val & IMAN_IE);
        IntrUpdate();
        return;

    case XHCI_IMOD:
        /* Stored and read back, but there is no clock here to moderate on. */
        fImod = val;
        return;

    case XHCI_ERSTSZ:
        fErstSz = val & 0xffff;
        if (fErstSz == 0) {
            fErSegSize = 0;
        } else if ((uint32_t)fErSegIndex >= fErstSz) {
            ResetEventRing();
        }
        return;

    case XHCI_ERSTBA_LO:
        fErstBaLo = val;
        return;

    case XHCI_ERSTBA_HI: {
        uint64_t base = (((uint64_t)val << 32) | fErstBaLo) & ~0x3fULL;
        if (base != fErstBa) {
            fErstBa = base;
            ResetEventRing();
        }
        return;
    }

    case XHCI_ERDP_LO:
        if ((val & ERDP_EHB) != 0) {
            fErdpLo &= ~ERDP_EHB;
        }
        fErdpLo = (val & ~ERDP_EHB) | (fErdpLo & ERDP_EHB);
        if ((val & ERDP_EHB) != 0) {
            /* Space has been freed: anything that had to wait goes out now,
               and an event ring that is still not empty interrupts again. */
            DrainEventFifo();
            uint64_t erdp = ((uint64_t)fErdpHi << 32) | (fErdpLo & ~0xfULL);
            if (fErSegSize != 0 && erdp >= fErSegStart &&
                erdp < fErSegStart + (uint64_t)fErSegSize * 16 &&
                erdp != fErEnqueue) {
                IntrRaise();
            }
        }
        return;

    case XHCI_ERDP_HI:
        fErdpHi = val;
        return;
    }
}


void XHCIDevice::DoorbellWrite(uint32_t offset, uint32_t val)
{
    int index = offset / 4;
    int target = val & 0xff;

    if (index == 0) {
        if (target == 0) {
            RunCommandRing();
        }
        return;
    }
    if (index > XHCI_MAX_SLOTS || target < 1 || target >= XHCI_DCI_COUNT) {
        return;
    }
    KickEndpoint(GetEndpoint(index, target));
}


uint32_t XHCIDevice::ReadDword(uint32_t offset)
{
    if (offset < XHCI_OPER_OFFSET) {
        return CapRead(offset);
    }
    if (offset < XHCI_PORT_OFFSET) {
        return OperRead(offset - XHCI_OPER_OFFSET);
    }
    if (offset < XHCI_RUNTIME_OFFSET) {
        return PortRead(offset - XHCI_PORT_OFFSET);
    }
    if (offset < XHCI_DOORBELL_OFFSET) {
        return RuntimeRead(offset - XHCI_RUNTIME_OFFSET);
    }
    if (offset < XHCI_MSIX_TABLE_OFFSET) {
        return 0; /* the doorbell array reads as zero */
    }
    if (offset < XHCI_MSIX_PBA_OFFSET) {
        return fMsix.TableRead(offset - XHCI_MSIX_TABLE_OFFSET, 2);
    }
    return fMsix.PbaRead(offset - XHCI_MSIX_PBA_OFFSET, 2);
}


void XHCIDevice::WriteDword(uint32_t offset, uint32_t val)
{
    if (offset < XHCI_OPER_OFFSET) {
        return; /* the capability registers are read only */
    }
    if (offset < XHCI_PORT_OFFSET) {
        OperWrite(offset - XHCI_OPER_OFFSET, val);
        return;
    }
    if (offset < XHCI_RUNTIME_OFFSET) {
        PortWrite(offset - XHCI_PORT_OFFSET, val);
        return;
    }
    if (offset < XHCI_DOORBELL_OFFSET) {
        RuntimeWrite(offset - XHCI_RUNTIME_OFFSET, val);
        return;
    }
    if (offset < XHCI_MSIX_TABLE_OFFSET) {
        DoorbellWrite(offset - XHCI_DOORBELL_OFFSET, val);
        return;
    }
    if (offset < XHCI_MSIX_PBA_OFFSET) {
        fMsix.TableWrite(offset - XHCI_MSIX_TABLE_OFFSET, val, 2);
        return;
    }
    /* The pending bit array is read only. */
}


uint32_t XHCIDevice::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val = ReadDword(offset & ~3u);

    if (size_log2 >= 2) {
        return val;
    }
    return (val >> ((offset & 3) * 8)) & ((1u << (8 << size_log2)) - 1);
}


void XHCIDevice::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (size_log2 >= 2) {
        WriteDword(offset & ~3u, val);
        return;
    }
    /* Neither target driver accesses these registers below a word, but
       merging is better than dropping the write on the floor. */
    int shift = (offset & 3) * 8;
    uint32_t mask = ((1u << (8 << size_log2)) - 1) << shift;
    uint32_t cur = ReadDword(offset & ~3u);
    WriteDword(offset & ~3u, (cur & ~mask) | ((val << shift) & mask));
}


void XHCIDevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    (void)bar_num;
    fMemRange->SetAddr(addr, enabled);
}


//#pragma mark - lifecycle

bool XHCIDevice::Prepare()
{
    if (ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    if (fPortCount < 1 || fPortCount > XHCI_MAX_PORTS) {
        vm_error("%s: %d ports is out of range\n", Name(), fPortCount);
        return false;
    }
    /* The BARs are placed by the guest, so no resources are declared. */
    fChildBus = new USBBus(this, this);
    return true;
}


bool XHCIDevice::Realize()
{
    PCIBus *bus = ParentBus()->AsPCIBus();

    /* Red Hat's identifiers for a plain xHCI controller. They matter: the
       NEC and Intel ones carry driver quirks that would oblige this to
       implement vendor commands or work around errata it does not have. */
    fPciDev = pci_register_device(bus, "xhci", -1, 0x1b36, 0x000d, 0x01,
                                  0x0c03);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI device\n", Name());
        return false;
    }
    /* The programming interface byte is what actually makes a driver bind. */
    pci_device_set_config8(fPciDev, PCI_CLASS_PROG, 0x30);
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    fMsix.Init(fPciDev, 0, 1, XHCI_MSIX_TABLE_OFFSET, XHCI_MSIX_PBA_OFFSET);

    fIrq = pci_device_get_irq(fPciDev, 0);
    PhysMemoryMap *mem_map = pci_device_get_mem_map(fPciDev);
    fMemRange = mem_map->RegisterDevice(0, XHCI_BAR_SIZE, this,
                                        DEVIO_SIZE8 | DEVIO_SIZE16 |
                                            DEVIO_SIZE32 | DEVIO_DISABLED);
    /* The specification defines BAR 0 as a 64 bit register pair, so it takes
       the slot after it too. */
    pci_register_bar(fPciDev, 0, XHCI_BAR_SIZE,
                     PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_MEM_TYPE_64,
                     this);

    /* Give every port its powered, empty state now. The devices declared
       below this one are attached as they are realized, and each of those
       calls PortUpdate() again for the port it landed on. */
    for (int i = 0; i < fPortCount; i++) {
        PortUpdate(i);
    }

    fUsbSts = USBSTS_HCH;
    return true;
}


//#pragma mark - factory

Device *xhci_node_create(const char *name, int usb2_ports, int usb3_ports)
{
    return new XHCIDevice(name, usb2_ports, usb3_ports);
}
