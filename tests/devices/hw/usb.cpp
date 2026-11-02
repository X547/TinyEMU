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
#include "usb.h"

#include "heap.h"
#include "mmio.h"
#include "test.h"

#define PCI_CLASS_XHCI 0x0c0330

void *usb_alloc(size_t size, size_t align)
{
    void *p = heap_alloc(size ? size : 1, align, 0x100000000ull);
    if (p == nullptr)
        t_skip("no memory below 4 GB");
    return p;
}

static void fence()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}


//#pragma mark - XhciRing

bool XhciRing::Init(uint32_t n)
{
    trbs = (Trb *)usb_alloc(n * sizeof(Trb), 64);
    size = n;
    enq = 0;
    cycle = true;
    Trb &link = trbs[n - 1];
    link.d[0] = (uint32_t)Base();
    link.d[1] = (uint32_t)(Base() >> 32);
    link.d[2] = 0;
    link.d[3] = TRB_TYPE(TRB_LINK) | TRB_TC; /* cycle 0: not ours yet */
    return true;
}

uint64_t XhciRing::Push(uint32_t p0, uint32_t p1, uint32_t status,
                        uint32_t control)
{
    Trb &t = trbs[enq];
    uint64_t addr = Enqueue();
    t.d[0] = p0;
    t.d[1] = p1;
    t.d[2] = status;
    fence();
    __atomic_store_n(&t.d[3], (control & ~TRB_C) | (cycle ? TRB_C : 0),
                     __ATOMIC_RELEASE);
    if (++enq == size - 1) {
        /* hand over the link and go round */
        Trb &link = trbs[size - 1];
        fence();
        __atomic_store_n(&link.d[3], (link.d[3] & ~TRB_C) |
                         (cycle ? TRB_C : 0), __ATOMIC_RELEASE);
        enq = 0;
        cycle = !cycle;
    }
    return addr;
}


//#pragma mark - Xhci

Xhci::Xhci(const HwDevice *d, PciFunc *f): dev(d), fn(f)
{
    if (dev != nullptr)
        base = hw_region(dev, HW_MMIO)->base;
}

uint32_t Xhci::Rd(uint32_t off)
{
    if (fn != nullptr)
        return pci_bar_read(fn, 0, off, 2);
    return mmio_read32(base + off);
}

void Xhci::Wr(uint32_t off, uint32_t v)
{
    if (fn != nullptr)
        pci_bar_write(fn, 0, off, v, 2);
    else
        mmio_write32(base + off, v);
}

void Xhci::Doorbell(uint32_t slot, uint32_t target)
{
    fence();
    Wr(db + 4 * slot, target);
}

void Xhci::ReadCaps()
{
    op = Rd(XHCI_CAPLENGTH) & 0xff;
    rt = Rd(XHCI_RTSOFF) & ~0x1fu;
    db = Rd(XHCI_DBOFF) & ~0x3u;
    uint32_t hcs1 = Rd(XHCI_HCSPARAMS1);
    max_slots = hcs1 & 0xff;
    max_intrs = (hcs1 >> 8) & 0x7ff;
    max_ports = hcs1 >> 24;
    uint32_t hcc1 = Rd(XHCI_HCCPARAMS1);
    ac64 = (hcc1 & 1) != 0;
    ctx_size = (hcc1 & 4) ? 64 : 32;
}

bool Xhci::Halt()
{
    OpWr(XHCI_USBCMD, OpRd(XHCI_USBCMD) & ~USBCMD_RS);
    running = false;
    return t_wait_until([&] {return (OpRd(XHCI_USBSTS) & USBSTS_HCH) != 0;},
                        100000);
}

bool Xhci::Reset()
{
    if (!(OpRd(XHCI_USBSTS) & USBSTS_HCH) && !Halt())
        return false;
    if (!t_wait_until([&] {return !(OpRd(XHCI_USBSTS) & USBSTS_CNR);},
                      1000000))
        return false;
    OpWr(XHCI_USBCMD, USBCMD_HCRST);
    return t_wait_until([&] {
        return !(OpRd(XHCI_USBCMD) & USBCMD_HCRST) &&
               !(OpRd(XHCI_USBSTS) & USBSTS_CNR);
    }, 1000000);
}

bool Xhci::Init(bool run, uint32_t cmd_size)
{
    ReadCaps();
    if (!Reset())
        return false;
    backlog_count = 0;

    OpWr(XHCI_CONFIG, max_slots);
    dcbaa = (uint64_t *)usb_alloc((max_slots + 1) * 8, 64);
    uint32_t hcs2 = Rd(XHCI_HCSPARAMS2);
    uint32_t scratch = ((hcs2 >> 21) & 0x1f) << 5 | ((hcs2 >> 27) & 0x1f);
    if (scratch != 0) {
        uint64_t *array = (uint64_t *)usb_alloc(scratch * 8, 64);
        for (uint32_t i = 0; i < scratch; i++)
            array[i] = (uintptr_t)usb_alloc(4096, 4096);
        dcbaa[0] = (uintptr_t)array;
    }
    OpWr64(XHCI_DCBAAP, (uintptr_t)dcbaa);

    cmd.Init(cmd_size);
    OpWr64(XHCI_CRCR, cmd.Base() | CRCR_RCS);

    /* the event ring (4.9.4), its table written last */
    event_count = XHCI_EVENTS;
    events = (Trb *)usb_alloc(event_count * sizeof(Trb), 64);
    event_deq = 0;
    event_cycle = true;
    erst = (uint8_t *)usb_alloc(64, 64);
    put_le32(erst, (uint32_t)(uintptr_t)events);
    put_le32(erst + 4, (uint32_t)((uint64_t)(uintptr_t)events >> 32));
    put_le32(erst + 8, event_count);
    RtWr(XHCI_ERSTSZ, 1);
    RtWr64(XHCI_ERDP, (uintptr_t)events);
    RtWr64(XHCI_ERSTBA, (uintptr_t)erst);
    return run ? Run() : true;
}

bool Xhci::Run()
{
    OpWr(XHCI_USBCMD, OpRd(XHCI_USBCMD) | USBCMD_RS);
    running = t_wait_until([&] {
        return !(OpRd(XHCI_USBSTS) & USBSTS_HCH);
    }, 100000);
    return running;
}

void Xhci::Cleanup()
{
    Reset();
    running = false;
}

bool Xhci::PollRing(Trb *ev)
{
    if (events == nullptr)
        return false;
    Trb *t = &events[event_deq];
    uint32_t control = __atomic_load_n(&t->d[3], __ATOMIC_ACQUIRE);
    if (((control & TRB_C) != 0) != event_cycle)
        return false;
    ev->d[0] = t->d[0];
    ev->d[1] = t->d[1];
    ev->d[2] = t->d[2];
    ev->d[3] = control;
    if (++event_deq == event_count) {
        event_deq = 0;
        event_cycle = !event_cycle;
    }
    RtWr64(XHCI_ERDP, ((uintptr_t)&events[event_deq]) | ERDP_EHB);
    return true;
}

bool Xhci::NextEvent(Trb *ev, uint64_t timeout_us)
{
    if (backlog_count > 0) {
        *ev = backlog[0];
        backlog_count--;
        memmove(backlog, backlog + 1, backlog_count * sizeof(Trb));
        return true;
    }
    return t_wait_until([&] {return PollRing(ev);}, timeout_us);
}

void Xhci::DrainEvents()
{
    Trb t;
    backlog_count = 0;
    while (PollRing(&t)) {
    }
}

int Xhci::TakePortEvents(uint32_t port)
{
    Trb t;
    while (PollRing(&t) && backlog_count < XHCI_BACKLOG)
        backlog[backlog_count++] = t;
    int n = 0;
    for (int i = 0; i < backlog_count;) {
        if (backlog[i].Type() == TRB_EV_PORT &&
            backlog[i].d[0] >> 24 == port) {
            backlog_count--;
            memmove(backlog + i, backlog + i + 1,
                    (backlog_count - i) * sizeof(Trb));
            n++;
        } else {
            i++;
        }
    }
    return n;
}

uint32_t Xhci::Command(uint32_t p0, uint32_t p1, uint32_t status,
                       uint32_t control, Trb *ev, uint64_t timeout_us)
{
    Trb local;
    if (ev == nullptr)
        ev = &local;
    uint64_t addr = cmd.Push(p0, p1, status, control);
    Doorbell(0, 0);
    if (!WaitEvent([addr](const Trb &t) {
            return t.Type() == TRB_EV_COMMAND && t.Param() == addr;
        }, ev, timeout_us))
        return CC_TIMEOUT;
    return ev->Cc();
}

uint32_t Xhci::EnableSlot(uint32_t *slot)
{
    Trb ev;
    uint32_t cc = Command(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ev);
    *slot = cc == CC_SUCCESS ? ev.Slot() : 0;
    return cc;
}

uint32_t Xhci::DisableSlot(uint32_t slot)
{
    return Command(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot));
}

void Xhci::PortSet(uint32_t port, uint32_t set)
{
    OpWr(XHCI_PORTSC(port), (Portsc(port) & PORTSC_KEEP) | set);
}

bool Xhci::PortReset(uint32_t port, bool warm)
{
    PortSet(port, warm ? PORTSC_WPR : PORTSC_PR);
    bool done = t_wait_until([&] {
        uint32_t v = Portsc(port);
        return !(v & PORTSC_PR) && (v & PORTSC_PRC);
    }, 500000);
    PortSet(port, Portsc(port) & PORTSC_CHANGES);
    TakePortEvents(port);
    return done && (Portsc(port) & PORTSC_PED);
}

uint32_t Xhci::FindExtCap(uint8_t id, int index)
{
    uint32_t off = (Rd(XHCI_HCCPARAMS1) >> 16) * 4;
    for (int n = 0; off != 0 && n < 64; n++) {
        uint32_t v = Rd(off);
        if ((v & 0xff) == id && index-- == 0)
            return off;
        uint32_t next = (v >> 8) & 0xff;
        off = next ? off + next * 4 : 0;
    }
    return 0;
}

int Xhci::PortMajor(uint32_t port)
{
    for (int i = 0;; i++) {
        uint32_t off = FindExtCap(XECP_PROTOCOL, i);
        if (off == 0)
            return 0;
        uint32_t ports = Rd(off + 8);
        uint32_t first = ports & 0xff, count = (ports >> 8) & 0xff;
        if (port >= first && port < first + count)
            return Rd(off) >> 24;
    }
}


//#pragma mark - discovery

namespace {

#define MAX_HOSTS 4

Xhci *sHosts[MAX_HOSTS];
int sHostCount = -1;

void discover()
{
    t_on_cleanup(usb_cleanup);
    sHostCount = 0;
    static const char *const kCompat[] = {"generic-xhci", "xhci-platform",
                                          "PNP0D10"};
    for (const char *compat : kCompat) {
        for (int i = 0; sHostCount < MAX_HOSTS; i++) {
            const HwDevice *dev = registry_find(compat, i);
            if (dev == nullptr)
                break;
            if (hw_region(dev, HW_MMIO) == nullptr)
                continue;
            bool dup = false;
            for (int k = 0; k < sHostCount; k++)
                dup |= sHosts[k]->dev == dev;
            if (!dup)
                sHosts[sHostCount++] = new Xhci(dev, nullptr);
        }
    }
    pci_enumerate();
    for (int i = 0; i < pci_func_count() && sHostCount < MAX_HOSTS; i++) {
        PciFunc *f = pci_func(i);
        if ((f->class_rev >> 8) == PCI_CLASS_XHCI && f->bars[0].size &&
            !f->bars[0].io && pci_reachable(f->host, f->bars[0].addr,
                                            f->bars[0].size, false))
            sHosts[sHostCount++] = new Xhci(nullptr, f);
    }
}

}

int xhci_count()
{
    if (sHostCount < 0)
        discover();
    return sHostCount;
}

Xhci *xhci_host(int index)
{
    return index < xhci_count() ? sHosts[index] : nullptr;
}

Xhci *xhci_require(int index)
{
    Xhci *hc = xhci_host(index);
    if (hc == nullptr)
        t_skip("no xHCI host %d", index);
    if (!hc->running && !hc->Init())
        t_skip("%s did not start", hc->Name());
    return hc;
}

void usb_cleanup()
{
    bool pci = false;
    for (int i = 0; i < sHostCount; i++) {
        if (sHosts[i]->dcbaa != nullptr || sHosts[i]->running)
            sHosts[i]->Cleanup();
        pci |= sHosts[i]->fn != nullptr;
    }
    if (pci)
        pci_restore_all();
    sHostCount = -1;
}


//#pragma mark - UsbDevice

uint32_t UsbDevice::SlotState()
{
    return le32(OutSlot() + 12) >> 27;
}

uint32_t UsbDevice::EpState(int dci)
{
    return le32(OutEp(dci)) & 7;
}

static uint32_t default_mps0(uint32_t speed)
{
    switch (speed) {
    case SPEED_LOW: return 8;
    case SPEED_SUPER: return 512;
    default: return 64;
    }
}

uint32_t UsbDevice::Address(bool bsr)
{
    uint32_t cc = hc->EnableSlot(&slot);
    if (cc != CC_SUCCESS)
        return cc;
    out = (uint8_t *)usb_alloc(XHCI_DCIS * hc->ctx_size, 64);
    in = (uint8_t *)usb_alloc((XHCI_DCIS + 1) * hc->ctx_size, 64);
    hc->dcbaa[slot] = (uintptr_t)out;
    fence();

    for (auto &r : rings)
        r = XhciRing();
    rings[1].Init(32);
    put_le32(InCtl() + 4, 0x3);
    put_le32(InSlot(), route | speed << 20 | 1u << 27);
    put_le32(InSlot() + 4, root_port << 16);
    put_le32(InSlot() + 8, tt_slot | tt_port << 8);
    put_le32(InEp(1) + 4, 3 << 1 | EPT_CONTROL << 3 |
             default_mps0(speed) << 16);
    put_le32(InEp(1) + 8, (uint32_t)rings[1].Base() | 1);
    put_le32(InEp(1) + 12, (uint32_t)(rings[1].Base() >> 32));
    put_le32(InEp(1) + 16, 8);
    fence();
    return hc->Command((uint32_t)(uintptr_t)in,
                       (uint32_t)((uint64_t)(uintptr_t)in >> 32), 0,
                       TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(slot) |
                       (bsr ? TRB_BSR : 0));
}

uint32_t UsbDevice::Control(uint8_t type, uint8_t req, uint16_t value,
                            uint16_t index, uint16_t len, void *buf,
                            uint32_t *actual)
{
    XhciRing &r = rings[1];
    bool in_dir = (type & USB_DIR_IN) != 0;
    uint32_t trt = len == 0 ? TRB_TRT_NONE : in_dir ? TRB_TRT_IN
                                                     : TRB_TRT_OUT;
    if (actual)
        *actual = 0;
    r.Push(type | req << 8 | (uint32_t)value << 16,
           index | (uint32_t)len << 16, 8,
           TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    uint64_t data = 0;
    if (len) {
        data = r.Push((uint32_t)(uintptr_t)buf,
                      (uint32_t)((uint64_t)(uintptr_t)buf >> 32), len,
                      TRB_TYPE(TRB_DATA) | TRB_ISP |
                      (in_dir ? TRB_DIR_IN : 0));
    }
    uint64_t status = r.Push(0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                             (len && in_dir ? 0 : TRB_DIR_IN));
    hc->Doorbell(slot, 1);

    uint32_t moved = len;
    for (;;) {
        Trb ev;
        uint32_t s = slot;
        if (!hc->WaitEvent([s](const Trb &t) {
                return t.Type() == TRB_EV_TRANSFER && t.Slot() == s &&
                       t.Ep() == 1;
            }, &ev, 2000000))
            return CC_TIMEOUT;
        uint32_t cc = ev.Cc();
        if (len && ev.Param() == data && cc == CC_SHORT_PACKET) {
            moved = len - ev.Residual();
            continue;
        }
        if (ev.Param() == status || cc != CC_SUCCESS) {
            if (actual)
                *actual = cc == CC_SUCCESS ? moved : 0;
            return cc;
        }
    }
}

uint32_t UsbDevice::GetDescriptor(uint8_t type, uint8_t index, void *buf,
                                  uint16_t len, uint32_t *actual)
{
    return Control(USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, type << 8 | index, 0,
                   len, buf, actual);
}

bool UsbDevice::ReadDescriptors()
{
    uint8_t *buf = (uint8_t *)usb_alloc(256);
    if (GetDescriptor(USB_DT_DEVICE, 0, buf, 8) != CC_SUCCESS)
        return false;
    uint32_t mps = speed == SPEED_SUPER ? 1u << buf[7] : buf[7];
    if (mps != default_mps0(speed)) {
        memset(in, 0, (XHCI_DCIS + 1) * hc->ctx_size);
        put_le32(InCtl() + 4, 0x2);
        memcpy(InEp(1), OutEp(1), hc->ctx_size);
        put_le32(InEp(1) + 4, (le32(OutEp(1) + 4) & 0xffff) | mps << 16);
        fence();
        if (hc->Command((uint32_t)(uintptr_t)in, 0, 0,
                        TRB_TYPE(TRB_EVALUATE_CTX) | TRB_SLOT(slot)) !=
            CC_SUCCESS)
            return false;
    }
    uint32_t got;
    if (GetDescriptor(USB_DT_DEVICE, 0, buf, 18, &got) != CC_SUCCESS ||
        got != 18)
        return false;
    memcpy(desc, buf, 18);
    if (GetDescriptor(USB_DT_CONFIG, 0, buf, 9) != CC_SUCCESS)
        return false;
    config_len = le16(buf + 2);
    config = (uint8_t *)usb_alloc(config_len);
    return GetDescriptor(USB_DT_CONFIG, 0, config, config_len, &got) ==
           CC_SUCCESS && got == config_len;
}

const uint8_t *usb_find_interface(const UsbDevice *dev, int n)
{
    for (uint32_t off = 0; off + 2 <= dev->config_len;) {
        const uint8_t *d = dev->config + off;
        if (d[0] < 2)
            break;
        if (d[1] == USB_DT_INTERFACE && d[3] == 0 && n-- == 0)
            return d;
        off += d[0];
    }
    return nullptr;
}

bool UsbDevice::FindEndpoint(int iface, int n, UsbEndpoint *ep)
{
    const uint8_t *id = usb_find_interface(this, iface);
    if (id == nullptr)
        return false;
    const uint8_t *end = config + config_len;
    for (const uint8_t *d = id + id[0]; d + 2 <= end && d[0] >= 2;
         d += d[0]) {
        if (d[1] == USB_DT_INTERFACE)
            break;
        if (d[1] == USB_DT_ENDPOINT && n-- == 0) {
            *ep = UsbEndpoint {d[2], d[3], le16(d + 4), d[6]};
            return true;
        }
    }
    return false;
}

static uint32_t ilog2(uint32_t v)
{
    uint32_t r = 0;
    while (v >>= 1)
        r++;
    return r;
}

void UsbDevice::SetupEpContext(const UsbEndpoint &ep)
{
    int dci = usb_dci(ep.address);
    bool is_in = (ep.address & USB_DIR_IN) != 0;
    uint32_t xfer = ep.attributes & 3;
    uint32_t type = (is_in ? 4 : 0) + xfer;
    bool periodic = xfer == 1 || xfer == 3;

    /* in 2^n microframes (6.2.3.6) */
    uint32_t interval = 0;
    if (periodic) {
        if (speed == SPEED_HIGH || speed == SPEED_SUPER || xfer == 1)
            interval = ep.interval - 1 + (speed == SPEED_FULL ? 3 : 0);
        else
            interval = ilog2(ep.interval * 8u);
    }
    rings[dci].Init(32);
    uint8_t *c = InEp(dci);
    memset(c, 0, hc->ctx_size);
    put_le32(c, interval << 16);
    put_le32(c + 4, (xfer == 1 ? 0 : 3u << 1) | type << 3 |
             (uint32_t)(ep.max_packet & 0x7ff) << 16);
    put_le32(c + 8, (uint32_t)rings[dci].Base() | 1);
    put_le32(c + 12, (uint32_t)(rings[dci].Base() >> 32));
    put_le32(c + 16, (periodic ? (uint32_t)ep.max_packet << 16 : 0) |
             (periodic ? ep.max_packet : 512));
    put_le32(InCtl() + 4, le32(InCtl() + 4) | 1u << dci);
}

uint32_t UsbDevice::Configure(uint8_t value)
{
    uint32_t cc = Control(0, USB_REQ_SET_CONFIGURATION, value, 0, 0,
                          nullptr);
    if (cc != CC_SUCCESS)
        return cc;
    memset(in, 0, (XHCI_DCIS + 1) * hc->ctx_size);
    memcpy(InSlot(), OutSlot(), hc->ctx_size);
    put_le32(InCtl() + 4, 1);
    int last = 1;
    for (int i = 0; usb_find_interface(this, i) != nullptr; i++) {
        UsbEndpoint ep;
        for (int n = 0; FindEndpoint(i, n, &ep); n++) {
            SetupEpContext(ep);
            last = max(last, usb_dci(ep.address));
        }
    }
    uint32_t dw0 = le32(InSlot()) & ~(0x1fu << 27);
    put_le32(InSlot(), dw0 | (uint32_t)last << 27 |
             (hub_ports ? 1u << 26 : 0));
    if (hub_ports)
        put_le32(InSlot() + 4, (le32(InSlot() + 4) & 0x00ffffff) |
                 (uint32_t)hub_ports << 24);
    put_le32(InSlot() + 12, 0);
    fence();
    return hc->Command((uint32_t)(uintptr_t)in,
                       (uint32_t)((uint64_t)(uintptr_t)in >> 32), 0,
                       TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(slot));
}

uint64_t UsbDevice::Queue(int dci, void *buf, uint32_t len, uint32_t flags)
{
    /* no TRB's buffer crosses a 64 KB boundary (6.4.1.1); all but the
       last are chained, and only the last interrupts on completion */
    XhciRing &r = rings[dci];
    uint64_t addr = (uintptr_t)buf;
    uint64_t first = r.Enqueue();
    td_count[dci] = 0;
    do {
        uint32_t n = min<uint64_t>(len, 0x10000 - (addr & 0xffff));
        bool last = n == len;
        uint64_t trb = r.Push((uint32_t)addr, (uint32_t)(addr >> 32), n,
                              TRB_TYPE(TRB_NORMAL) |
                              (last ? flags : (flags & ~TRB_IOC) | TRB_CH));
        if (td_count[dci] < USB_MAX_TD_TRBS) {
            td_trb[dci][td_count[dci]] = trb;
            td_len[dci][td_count[dci]++] = n;
        }
        addr += n;
        len -= n;
    } while (len > 0);
    hc->Doorbell(slot, dci);
    return first;
}

uint32_t UsbDevice::WaitTransfer(int dci, uint32_t *residual,
                                 uint64_t timeout_us, Trb *ev)
{
    Trb local;
    if (ev == nullptr)
        ev = &local;
    uint32_t s = slot;
    uint32_t e = dci;
    if (!hc->WaitEvent([s, e](const Trb &t) {
            return t.Type() == TRB_EV_TRANSFER && t.Slot() == s &&
                   t.Ep() == e;
        }, ev, timeout_us))
        return CC_TIMEOUT;
    if (residual)
        *residual = ev->Residual();
    return ev->Cc();
}

uint32_t UsbDevice::Transfer(int dci, void *buf, uint32_t len,
                             uint32_t *actual, uint64_t timeout_us)
{
    Queue(dci, buf, len);
    Trb ev;
    uint32_t cc = WaitTransfer(dci, nullptr, timeout_us, &ev);
    if (actual == nullptr)
        return cc;
    *actual = 0;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)
        return cc;
    /* what the TRBs before the one reported moved, and that one's part */
    for (int i = 0; i < td_count[dci]; i++) {
        if (td_trb[dci][i] == ev.Param()) {
            *actual += td_len[dci][i] - ev.Residual();
            break;
        }
        *actual += td_len[dci][i];
    }
    return cc;
}

uint32_t UsbDevice::StopEndpoint(int dci, Trb *ev)
{
    return hc->Command(0, 0, 0, TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(slot) |
                       TRB_EP(dci), ev);
}

uint32_t UsbDevice::ResetEndpoint(int dci)
{
    return hc->Command(0, 0, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(slot) |
                       TRB_EP(dci));
}

uint32_t UsbDevice::SkipQueued(int dci)
{
    uint64_t deq = rings[dci].Enqueue() | (rings[dci].cycle ? 1 : 0);
    return hc->Command((uint32_t)deq, (uint32_t)(deq >> 32), 0,
                       TRB_TYPE(TRB_SET_TR_DEQ) | TRB_SLOT(slot) |
                       TRB_EP(dci));
}

bool UsbDevice::Recover(int dci)
{
    if (ResetEndpoint(dci) != CC_SUCCESS || SkipQueued(dci) != CC_SUCCESS)
        return false;
    if (dci == 1)
        return true;
    uint8_t addr = dci / 2 | (dci & 1 ? USB_DIR_IN : 0);
    return Control(USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0, addr, 0,
                   nullptr) == CC_SUCCESS;
}


//#pragma mark - enumeration

namespace {

/* hub class requests (USB 2.0 11.24.2) */
#define HUB_PORT_TYPE  (USB_TYPE_CLASS | USB_RECIP_OTHER)
#define HUB_PORT_RESET 4
#define HUB_PORT_POWER 8
#define HUB_C_CONNECTION 16
#define HUB_C_RESET    20

uint8_t device_class(const UsbDevice &d)
{
    if (d.desc[4] != 0)
        return d.desc[4];
    const uint8_t *id = usb_find_interface(&d, 0);
    return id != nullptr ? id[5] : 0;
}

int route_depth(uint32_t route)
{
    int n = 0;
    while (n < 5 && ((route >> (4 * n)) & 0xf) != 0)
        n++;
    return n;
}

struct Walk {
    uint8_t cls;
    int index;
    UsbDevice *found;
    int max;    /* collecting every device: room in 'found' */
    int count;
};

bool visit(UsbDevice &d, Walk &w);

/* Configure a hub and address what its ports have (USB 2.0 11.24). */
bool walk_hub(UsbDevice &hub, Walk &w)
{
    uint8_t *buf = (uint8_t *)usb_alloc(16);
    if (hub.Control(USB_DIR_IN | USB_TYPE_CLASS, USB_REQ_GET_DESCRIPTOR,
                    USB_DT_HUB << 8, 0, 9, buf) != CC_SUCCESS)
        return false;
    hub.hub_ports = buf[2];
    if (hub.Configure() != CC_SUCCESS)
        return false;
    for (int p = 1; p <= hub.hub_ports; p++)
        hub.Control(HUB_PORT_TYPE, USB_REQ_SET_FEATURE, HUB_PORT_POWER, p, 0,
                    nullptr);
    for (int p = 1; p <= hub.hub_ports; p++) {
        if (hub.Control(USB_DIR_IN | HUB_PORT_TYPE, USB_REQ_GET_STATUS, 0, p,
                        4, buf) != CC_SUCCESS || !(buf[0] & 1))
            continue;
        hub.Control(HUB_PORT_TYPE, USB_REQ_SET_FEATURE, HUB_PORT_RESET, p, 0,
                    nullptr);
        bool reset = t_wait_until([&] {
            return hub.Control(USB_DIR_IN | HUB_PORT_TYPE, USB_REQ_GET_STATUS,
                               0, p, 4, buf) == CC_SUCCESS &&
                   (buf[2] & 0x10) != 0;
        }, 500000);
        hub.Control(HUB_PORT_TYPE, USB_REQ_CLEAR_FEATURE, HUB_C_RESET, p, 0,
                    nullptr);
        hub.Control(HUB_PORT_TYPE, USB_REQ_CLEAR_FEATURE, HUB_C_CONNECTION, p,
                    0, nullptr);
        if (!reset || !(buf[0] & 2))
            continue;
        UsbDevice child {};
        child.hc = hub.hc;
        child.root_port = hub.root_port;
        child.route = hub.route | (uint32_t)p << (4 * route_depth(hub.route));
        child.speed = (buf[1] & 2) ? SPEED_LOW : (buf[1] & 4) ? SPEED_HIGH
                                                               : SPEED_FULL;
        if (hub.speed == SPEED_HIGH && child.speed != SPEED_HIGH) {
            child.tt_slot = hub.slot;
            child.tt_port = p;
        } else {
            child.tt_slot = hub.tt_slot;
            child.tt_port = hub.tt_port;
        }
        if (child.Address() != CC_SUCCESS || !child.ReadDescriptors())
            continue;
        if (visit(child, w))
            return true;
    }
    return false;
}

bool visit(UsbDevice &d, Walk &w)
{
    uint8_t cls = device_class(d);
    if (w.max > 0) {
        /* a hub is kept as walking it left it, configured */
        if (cls == USB_CLASS_HUB)
            walk_hub(d, w);
        if (w.count < w.max)
            w.found[w.count++] = d;
        return false;
    }
    if ((w.cls == 0 || cls == w.cls) && w.index-- == 0) {
        *w.found = d;
        return true;
    }
    if (cls == USB_CLASS_HUB)
        return walk_hub(d, w);
    return false;
}

}

static bool walk(Walk &w, int host)
{
    for (int h = 0; h < xhci_count(); h++) {
        if (host >= 0 && h != host)
            continue;
        Xhci *hc = xhci_host(h);
        if (!hc->running && !hc->Init())
            continue;
        for (uint32_t p = 1; p <= hc->max_ports; p++) {
            if (!(hc->Portsc(p) & PORTSC_CCS))
                continue;
            if (!(hc->Portsc(p) & PORTSC_PED) && !hc->PortReset(p))
                continue;
            UsbDevice d {};
            d.hc = hc;
            d.root_port = p;
            d.speed = PORTSC_SPEED(hc->Portsc(p));
            if (d.Address() != CC_SUCCESS || !d.ReadDescriptors())
                continue;
            if (visit(d, w))
                return true;
        }
    }
    return false;
}

bool usb_find_device(uint8_t cls, int index, UsbDevice *dev, int host)
{
    Walk w {cls, index, dev, 0, 0};
    return walk(w, host);
}

int usb_find_all(UsbDevice *devs, int max, int host)
{
    Walk w {0, 0, devs, max, 0};
    walk(w, host);
    return w.count;
}

void usb_require_device(uint8_t cls, int index, UsbDevice *dev, int host)
{
    if (!usb_find_device(cls, index, dev, host))
        t_skip("no USB device of class %#x", cls);
}


//#pragma mark - UsbBot

#define CBW_SIGNATURE 0x43425355
#define CSW_SIGNATURE 0x53425355

bool UsbBot::Open(int index)
{
    if (!usb_find_device(USB_CLASS_STORAGE, index, &dev))
        return false;
    for (int i = 0;; i++) {
        const uint8_t *id = usb_find_interface(&dev, i);
        if (id == nullptr)
            return false;
        if (id[5] == USB_CLASS_STORAGE && id[7] == 0x50) {
            iface = id[2];
            UsbEndpoint ep;
            for (int n = 0; dev.FindEndpoint(i, n, &ep); n++) {
                if ((ep.attributes & 3) != 2)
                    continue;
                if (ep.address & USB_DIR_IN)
                    in = usb_dci(ep.address);
                else
                    out = usb_dci(ep.address);
            }
            break;
        }
    }
    cbw = (uint8_t *)usb_alloc(32);
    csw = (uint8_t *)usb_alloc(16);
    return in && out && dev.Configure() == CC_SUCCESS;
}

int UsbBot::MaxLun()
{
    uint8_t *buf = (uint8_t *)usb_alloc(1);
    uint32_t cc = dev.Control(USB_DIR_IN | USB_TYPE_CLASS |
                              USB_RECIP_INTERFACE, 0xfe, 0, iface, 1, buf);
    if (cc != CC_SUCCESS) {
        dev.Recover(1);
        return -1;
    }
    return buf[0];
}

bool UsbBot::ResetRecovery()
{
    if (dev.Control(USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0xff, 0, iface, 0,
                    nullptr) != CC_SUCCESS)
        return false;
    const int dcis[2] = {in, out};
    for (int dci : dcis) {
        if (dev.EpState(dci) == EP_HALTED) {
            if (!dev.Recover(dci))
                return false;
        } else {
            uint8_t addr = dci / 2 | (dci & 1 ? USB_DIR_IN : 0);
            if (dev.Control(USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0,
                            addr, 0, nullptr) != CC_SUCCESS)
                return false;
        }
    }
    return true;
}

uint32_t UsbBot::Command(uint8_t lun, const uint8_t *cdb, int cdb_len,
                         bool is_in, void *buf, uint32_t len,
                         uint32_t *residue)
{
    memset(cbw, 0, 31);
    put_le32(cbw, CBW_SIGNATURE);
    put_le32(cbw + 4, ++tag);
    put_le32(cbw + 8, len);
    cbw[12] = is_in && len ? 0x80 : 0;
    cbw[13] = lun;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    uint32_t cc = dev.Transfer(out, cbw, 31);
    if (cc != CC_SUCCESS) {
        if (cc == CC_STALL)
            dev.Recover(out);
        return BOT_TRANSPORT;
    }
    if (len) {
        int dci = is_in ? in : out;
        cc = dev.Transfer(dci, buf, len, nullptr, 10000000);
        if (cc == CC_STALL)
            dev.Recover(dci);
        else if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)
            return BOT_TRANSPORT;
    }
    /* a stalled status stage is tried once more after the halt is
       cleared (6.7.2) */
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(csw, 0, 13);
        uint32_t got;
        cc = dev.Transfer(in, csw, 13, &got, 10000000);
        if (cc == CC_STALL) {
            dev.Recover(in);
            continue;
        }
        if ((cc != CC_SUCCESS && cc != CC_SHORT_PACKET) || got != 13 ||
            le32(csw) != CSW_SIGNATURE || le32(csw + 4) != tag)
            return BOT_TRANSPORT;
        if (residue)
            *residue = le32(csw + 8);
        return csw[12];
    }
    return BOT_TRANSPORT;
}
