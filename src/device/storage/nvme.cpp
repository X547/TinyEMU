/*
 * NVM Express controller
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
#include "nvme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "machine.h"
#include "pci.h"
#include "virtio.h"

//#define DEBUG_NVME

#ifdef DEBUG_NVME
#define nvme_debug(...) fprintf(stderr, "nvme: " __VA_ARGS__)
#else
#define nvme_debug(...) do {} while (0)
#endif


//#pragma mark - register and command definitions

/* Controller registers, at the start of BAR 0. */
#define NVME_CAP_LO     0x00
#define NVME_CAP_HI     0x04
#define NVME_VS         0x08
#define NVME_INTMS      0x0c
#define NVME_INTMC      0x10
#define NVME_CC         0x14
#define NVME_CSTS       0x1c
#define NVME_NSSR       0x20
#define NVME_AQA        0x24
#define NVME_ASQ_LO     0x28
#define NVME_ASQ_HI     0x2c
#define NVME_ACQ_LO     0x30
#define NVME_ACQ_HI     0x34

#define NVME_DOORBELL_OFFSET   0x1000
#define NVME_MSIX_TABLE_OFFSET 0x2000
#define NVME_MSIX_PBA_OFFSET   0x3000
#define NVME_BAR_SIZE          0x4000

#define CC_EN           (1 << 0)
#define CC_CSS_SHIFT    4
#define CC_MPS_SHIFT    7
#define CC_MPS_MASK     0xf
#define CC_SHN_SHIFT    14
#define CC_SHN_MASK     0x3
#define CC_IOSQES_SHIFT 16
#define CC_IOCQES_SHIFT 20

#define CSTS_RDY        (1 << 0)
#define CSTS_CFS        (1 << 1)
#define CSTS_SHST_SHIFT 2
#define CSTS_SHST_MASK  (0x3 << 2)

/* How many entries a queue may have, and how many pairs the controller
   offers. MQES in CAP is reported one less, as the specification asks. */
#define NVME_MAX_QUEUE_ENTRIES 1024
#define NVME_MAX_IO_QUEUES 8
#define NVME_MAX_QUEUES (NVME_MAX_IO_QUEUES + 1) /* queue 0 is the admin pair */

#define NVME_MSIX_VECTORS 16

#define NVME_SQE_SIZE 64
#define NVME_CQE_SIZE 16

/* The largest transfer one command may ask for, as a power of two multiple of
   the minimum page size. Five means 128 KB, which needs at most one page of
   PRP list entries; chaining is implemented anyway. */
#define NVME_MDTS 5
#define NVME_MAX_TRANSFER (4096u << NVME_MDTS)

/* Admin command opcodes. */
#define NVME_ADM_DELETE_SQ  0x00
#define NVME_ADM_CREATE_SQ  0x01
#define NVME_ADM_GET_LOG    0x02
#define NVME_ADM_DELETE_CQ  0x04
#define NVME_ADM_CREATE_CQ  0x05
#define NVME_ADM_IDENTIFY   0x06
#define NVME_ADM_ABORT      0x08
#define NVME_ADM_SET_FEAT   0x09
#define NVME_ADM_GET_FEAT   0x0a
#define NVME_ADM_ASYNC_EVENT 0x0c
#define NVME_ADM_KEEP_ALIVE 0x18

/* I/O command opcodes. */
#define NVME_IO_FLUSH       0x00
#define NVME_IO_WRITE       0x01
#define NVME_IO_READ        0x02
#define NVME_IO_WRITE_ZEROES 0x08
#define NVME_IO_DSM         0x09

/* Identify CNS values. */
#define NVME_CNS_NAMESPACE  0x00
#define NVME_CNS_CONTROLLER 0x01
#define NVME_CNS_NS_LIST    0x02
#define NVME_CNS_NS_DESC    0x03

/* Feature identifiers. */
#define NVME_FEAT_NUM_QUEUES  0x07
#define NVME_FEAT_ASYNC_EVENT 0x0b

/* Status code types. */
#define NVME_SCT_GENERIC  0
#define NVME_SCT_SPECIFIC 1

/* Generic status codes. */
#define NVME_SC_SUCCESS         0x00
#define NVME_SC_INVALID_OPCODE  0x01
#define NVME_SC_INVALID_FIELD   0x02
#define NVME_SC_DATA_XFER_ERROR 0x04
#define NVME_SC_INTERNAL_ERROR  0x06
#define NVME_SC_INVALID_NS      0x0b
#define NVME_SC_LBA_RANGE       0x80

/* Command specific status codes. */
#define NVME_SC_CQ_INVALID      0x00
#define NVME_SC_QID_INVALID     0x01
#define NVME_SC_QSIZE_INVALID   0x02
#define NVME_SC_AER_LIMIT       0x05
#define NVME_SC_INVALID_VECTOR  0x08
#define NVME_SC_QUEUE_DELETION  0x0c

/* How many Async Event Requests are held outstanding before the limit is
   reported. They are never completed: no event is ever generated here, which
   is exactly what a real controller does until something happens. */
#define NVME_MAX_AERS 4


/* One submission queue entry, as it sits in guest memory. */
struct NVMeCommand {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;

    uint8_t Opcode() const {return cdw0 & 0xff;}
    uint16_t CommandId() const {return cdw0 >> 16;}
};


struct NVMeSubQueue {
    bool enabled = false;
    uint64_t base = 0;
    uint32_t size = 0; /* entries */
    uint32_t head = 0;
    uint32_t tail = 0;
    uint16_t cqid = 0;
};


struct NVMeCompQueue {
    bool enabled = false;
    uint64_t base = 0;
    uint32_t size = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    bool phase = true;
    bool ien = false;
    uint16_t vector = 0;
};


class NVMeDevice;


//#pragma mark - NVMeNamespace

/* One namespace: a block back end plus the identity the guest reads from it.
   Logical blocks are 512 bytes, which is what the backing BlockDevice counts
   in, so no translation is needed. */
class NVMeNamespace {
private:
    std::unique_ptr<BlockDevice> fBlockDev;
    uint32_t fNsid = 0;

public:
    NVMeNamespace(std::unique_ptr<BlockDevice> bs): fBlockDev(std::move(bs)) {}

    BlockDevice *Backend() const {return fBlockDev.get();}
    uint32_t Nsid() const {return fNsid;}
    void SetNsid(uint32_t nsid) {fNsid = nsid;}

    uint64_t BlockCount() const {return fBlockDev->SectorCount();}
    static uint32_t BlockSize() {return 512;}
};


/* Implemented by the controller, so a namespace node can attach itself. */
class NVMeNamespaceTarget {
public:
    virtual ~NVMeNamespaceTarget() = default;

    /* The lowest unused namespace id, or 0 when the controller is full. */
    virtual int FindFreeNsid() = 0;
    virtual bool AttachNamespace(NVMeNamespace *ns, uint32_t nsid) = 0;
};


/* The bus a controller provides. Like the USB and SCSI buses it hands out
   identifiers rather than host address space, so it assigns no resources. */
class NVMeBus final: public Bus {
private:
    NVMeNamespaceTarget *fTarget;

public:
    NVMeBus(Device *owner, NVMeNamespaceTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "nvme";}
    NVMeNamespaceTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override
    {
        for (int i = 0; i < dev->ResourceCount(); i++) {
            if (dev->ResourceAt(i)->type != RES_NONE) {
                vm_error("nvme bus: device '%s' declared a resource, but a "
                         "namespace has none of its own\n", dev->Name());
                return false;
            }
        }
        return true;
    }
};


/* Attaches one namespace to the controller it was declared under. */
class NVMeNamespaceNode final: public Device {
private:
    std::unique_ptr<NVMeNamespace> fNs;
    int fNsid; /* < 0 asks for the first free id */

public:
    NVMeNamespaceNode(std::unique_ptr<NVMeNamespace> ns, int nsid):
        Device("nvme-ns"), fNs(std::move(ns)), fNsid(nsid) {}

    bool Realize() override
    {
        NVMeBus *bus = dynamic_cast<NVMeBus *>(ParentBus());
        if (bus == nullptr) {
            vm_error("%s: must be attached to an NVMe controller\n", Name());
            return false;
        }
        int nsid = fNsid;
        if (nsid < 0) {
            nsid = bus->Target()->FindFreeNsid();
            if (nsid == 0) {
                vm_error("%s: no free namespace id\n", Name());
                return false;
            }
        }
        return bus->Target()->AttachNamespace(fNs.get(), nsid);
    }
};


//#pragma mark - NVMeDevice

class NVMeDevice final: public Device, public PCIBarTarget, public DeviceIO,
                        public NVMeNamespaceTarget {
private:
    /* The block back end answers through this when it takes a request
       asynchronously; the controller holds the command until it does. */
    class Completion final: public BlockDeviceCompletion {
    private:
        NVMeDevice &fCtrl;

    public:
        Completion(NVMeDevice &ctrl): fCtrl(ctrl) {}

        void Complete(int ret) override {fCtrl.BlockDone(ret);}
    };

    uint32_t fQuirks;

    PCIDevice *fPciDev = nullptr;
    PhysMemoryRange *fMemRange = nullptr;
    IRQSignal *fIrq = nullptr;
    PCIMsixState fMsix {};
    bool fIrqLevel = false;

    std::unique_ptr<NVMeBus> fChildBus;
    NVMeNamespace *fNamespaces[NVME_MAX_NAMESPACES + 1] {}; /* 1 based */
    uint32_t fMaxNsid = 0;

    /* registers */
    uint32_t fCc = 0;
    uint32_t fCsts = 0;
    uint32_t fIntMask = 0;
    uint32_t fAqa = 0;
    uint32_t fAsqLo = 0;
    uint64_t fAsq = 0;
    uint32_t fAcqLo = 0;
    uint64_t fAcq = 0;

    NVMeSubQueue fSq[NVME_MAX_QUEUES] {};
    NVMeCompQueue fCq[NVME_MAX_QUEUES] {};
    int fAerCount = 0;
    uint32_t fNumSqAllocated = NVME_MAX_IO_QUEUES;
    uint32_t fNumCqAllocated = NVME_MAX_IO_QUEUES;

    /* One command may be with the block back end at a time; while it is, no
       queue is served, so the controller never has two in flight. */
    Completion fCompletion {*this};
    bool fBusy = false;
    uint16_t fPendingCid = 0;
    uint16_t fPendingSqid = 0;
    uint16_t fPendingCqid = 0;
    bool fPendingIsRead = false;
    uint64_t fPendingPrp1 = 0;
    uint64_t fPendingPrp2 = 0;
    uint32_t fPendingLength = 0;

    uint8_t *fBuf = nullptr;
    uint32_t fBufSize = 0;

    uint32_t PageSize() const
    {
        return 1u << (12 + ((fCc >> CC_MPS_SHIFT) & CC_MPS_MASK));
    }

    /* True when the admin queue may be served. The specification says this
       needs CC.EN; a guest that never enables the controller needs the
       quirk. */
    bool AdminReady() const
    {
        if ((fCsts & CSTS_RDY) != 0) {
            return true;
        }
        return (fQuirks & NVME_QUIRK_NO_ENABLE_CHECK) != 0 && fAsq != 0 &&
               fAcq != 0 && fAqa != 0;
    }

    bool EnsureBuffer(uint32_t size);
    NVMeNamespace *NamespaceFor(uint32_t nsid);

    /* register file */
    uint32_t ReadDword(uint32_t offset);
    void WriteDword(uint32_t offset, uint32_t val);
    void DoorbellWrite(uint32_t offset, uint32_t val);
    void ControllerEnable();
    void ControllerDisable();

    /* queues */
    void ArmAdminQueue();
    void ProcessSq(int sqid);
    void PostCompletion(uint16_t sqid, uint16_t cqid, uint16_t cid,
                        uint32_t dw0, int sct, int sc);
    void UpdateIrq();

    /* commands */
    void Execute(int sqid, const NVMeCommand &cmd);
    void ExecuteAdmin(const NVMeCommand &cmd);
    void ExecuteIo(int sqid, const NVMeCommand &cmd);
    int CmdCreateCq(const NVMeCommand &cmd);
    int CmdCreateSq(const NVMeCommand &cmd);
    int CmdIdentify(const NVMeCommand &cmd);
    int CmdGetLogPage(const NVMeCommand &cmd);
    int CmdSetFeatures(const NVMeCommand &cmd, uint32_t *dw0);
    int CmdGetFeatures(const NVMeCommand &cmd, uint32_t *dw0);

    void IdentifyController(uint8_t *buf);
    void IdentifyNamespace(uint8_t *buf, NVMeNamespace *ns);

    void BlockDone(int ret);

public:
    NVMeDevice(const char *name, uint32_t quirks):
        Device(name), fQuirks(quirks) {}
    ~NVMeDevice() override;

    /* DMA, chunked a page at a time, as pci_device_get_dma_ptr requires. */
    bool DmaRead(uint64_t addr, void *buf, uint32_t len);
    bool DmaWrite(uint64_t addr, const void *buf, uint32_t len);
    /* Move a transfer between a host buffer and the pages a command's PRP
       entries name. */
    bool PrpTransfer(uint64_t prp1, uint64_t prp2, void *host, uint32_t len,
                     bool to_guest);

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus.get();}

    void SetBar(int bar_num, uint64_t addr, bool enabled) override;
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    int FindFreeNsid() override;
    bool AttachNamespace(NVMeNamespace *ns, uint32_t nsid) override;
};


NVMeDevice::~NVMeDevice()
{
    free(fBuf);
}


//#pragma mark - DMA and PRP

bool NVMeDevice::DmaRead(uint64_t addr, void *buf, uint32_t len)
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


bool NVMeDevice::DmaWrite(uint64_t addr, const void *buf, uint32_t len)
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


/* Walk the PRP entries of one command. The first may start part way into a
   page; every entry after it is page aligned. Up to two pages are named
   directly by PRP1 and PRP2; beyond that PRP2 points at a list, whose last
   entry chains to another list when more than one page still remains. */
bool NVMeDevice::PrpTransfer(uint64_t prp1, uint64_t prp2, void *host,
                             uint32_t len, bool to_guest)
{
    uint8_t *hp = static_cast<uint8_t *>(host);
    uint32_t page_size = PageSize();
    uint32_t offset = prp1 & (page_size - 1);

    /* The page PRP1 points into, from wherever in it the transfer starts. */
    uint32_t chunk = page_size - offset;
    if (chunk > len) {
        chunk = len;
    }
    bool ok = to_guest ? DmaWrite(prp1, hp, chunk) : DmaRead(prp1, hp, chunk);
    if (!ok) {
        return false;
    }
    hp += chunk;
    len -= chunk;
    if (len == 0) {
        return true;
    }

    if (len <= page_size) {
        /* PRP2 names the one remaining page. */
        return to_guest ? DmaWrite(prp2, hp, len) : DmaRead(prp2, hp, len);
    }

    /* PRP2 names a list of pages. A list runs from wherever it starts to the
       end of the page containing it, so how many entries it holds depends on
       its offset -- a host may put one part way into a page, and computing
       the length from the page size alone would then find the chaining entry
       in the wrong place. */
    uint64_t list = prp2;
    int guard = 0;

    while (len > 0) {
        uint32_t entries =
            (uint32_t)((page_size - (list & (page_size - 1))) / 8);
        for (uint32_t i = 0; i < entries && len > 0; i++) {
            uint8_t entry[8];
            if (!DmaRead(list + (uint64_t)i * 8, entry, 8)) {
                return false;
            }
            uint64_t page = get_le64(entry);

            if (i == entries - 1 && len > page_size) {
                /* The last entry of a list continues it elsewhere, but only
                   when more than one page still remains. */
                if (++guard > 64) {
                    vm_error("nvme: PRP list chain does not end\n");
                    return false;
                }
                list = page;
                break;
            }

            chunk = len < page_size ? len : page_size;
            ok = to_guest ? DmaWrite(page, hp, chunk)
                          : DmaRead(page, hp, chunk);
            if (!ok) {
                return false;
            }
            hp += chunk;
            len -= chunk;
        }
    }
    return true;
}


bool NVMeDevice::EnsureBuffer(uint32_t size)
{
    if (size <= fBufSize) {
        return true;
    }
    uint8_t *buf = static_cast<uint8_t *>(realloc(fBuf, size));
    if (buf == nullptr) {
        return false;
    }
    fBuf = buf;
    fBufSize = size;
    return true;
}


NVMeNamespace *NVMeDevice::NamespaceFor(uint32_t nsid)
{
    /* A guest that addresses namespace 0 means namespace 1 only when it has
       been declared quirky; the specification reserves 0. */
    if (nsid == 0 && (fQuirks & NVME_QUIRK_NSID_ZERO) != 0) {
        nsid = 1;
    }
    if (nsid < 1 || nsid > NVME_MAX_NAMESPACES) {
        return nullptr;
    }
    return fNamespaces[nsid];
}


//#pragma mark - namespaces

int NVMeDevice::FindFreeNsid()
{
    for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        if (fNamespaces[i] == nullptr) {
            return i;
        }
    }
    return 0;
}


bool NVMeDevice::AttachNamespace(NVMeNamespace *ns, uint32_t nsid)
{
    if (nsid < 1 || nsid > NVME_MAX_NAMESPACES) {
        vm_error("%s: namespace id %u is out of range\n", Name(), nsid);
        return false;
    }
    if (fNamespaces[nsid] != nullptr) {
        vm_error("%s: namespace %u already exists\n", Name(), nsid);
        return false;
    }
    fNamespaces[nsid] = ns;
    ns->SetNsid(nsid);
    if (nsid > fMaxNsid) {
        fMaxNsid = nsid;
    }
    return true;
}


//#pragma mark - queues and completions

void NVMeDevice::ArmAdminQueue()
{
    fSq[0].base = fAsq;
    fSq[0].size = (fAqa & 0xfff) + 1;
    fSq[0].head = 0;
    fSq[0].tail = 0;
    fSq[0].cqid = 0;
    fSq[0].enabled = true;

    fCq[0].base = fAcq;
    fCq[0].size = ((fAqa >> 16) & 0xfff) + 1;
    fCq[0].head = 0;
    fCq[0].tail = 0;
    fCq[0].phase = true;
    fCq[0].ien = true;
    fCq[0].vector = 0;
    fCq[0].enabled = true;
}


void NVMeDevice::PostCompletion(uint16_t sqid, uint16_t cqid, uint16_t cid,
                                uint32_t dw0, int sct, int sc)
{
    if (cqid >= NVME_MAX_QUEUES || !fCq[cqid].enabled) {
        return;
    }
    NVMeCompQueue *cq = &fCq[cqid];
    uint8_t entry[NVME_CQE_SIZE];

    /* Overwriting an entry the host has not read yet turns a guest's mistake
       into silent corruption of its own memory, so a full queue is reported
       rather than written through. A conformant host never has more commands
       outstanding than the queue can hold. */
    uint32_t next = cq->tail + 1;
    if (next >= cq->size) {
        next = 0;
    }
    if (next == cq->head) {
        vm_error("nvme: completion queue %u is full\n", cqid);
        fCsts |= CSTS_CFS;
        return;
    }

    put_le32(entry, dw0);
    put_le32(entry + 4, 0);
    put_le32(entry + 8, fSq[sqid].head | ((uint32_t)sqid << 16));
    /* The phase bit is what publishes the entry, so the dword carrying it is
       written last. */
    uint32_t status = ((uint32_t)sc << 17) | ((uint32_t)sct << 25);
    put_le32(entry + 12, cid | (cq->phase ? (1u << 16) : 0) | status);

    if (!DmaWrite(cq->base + (uint64_t)cq->tail * NVME_CQE_SIZE, entry, 12) ||
        !DmaWrite(cq->base + (uint64_t)cq->tail * NVME_CQE_SIZE + 12,
                  entry + 12, 4)) {
        fCsts |= CSTS_CFS;
        return;
    }

    cq->tail++;
    if (cq->tail >= cq->size) {
        cq->tail = 0;
        cq->phase = !cq->phase;
    }

    if (cq->ien) {
        if (fMsix.Enabled()) {
            fMsix.Send(cq->vector);
        }
    }
    UpdateIrq();
}


void NVMeDevice::UpdateIrq()
{
    if (fIrq == nullptr) {
        return;
    }
    if (fMsix.Enabled()) {
        /* A message is an edge, sent when the entry is posted; the pin must
           stay low while MSI-X is in use. */
        fIrq->Set(0);
        fIrqLevel = false;
        return;
    }

    /* A driver that means to poll turns the pin off in configuration space
       rather than in the controller, and a level line that keeps firing an
       unhandled interrupt will wedge the guest. */
    if ((fQuirks & NVME_QUIRK_POLL_ONLY) != 0 ||
        (pci_device_get_config(fPciDev, PCI_COMMAND, 1) &
         PCI_COMMAND_INTX_DISABLE) != 0) {
        if (fIrqLevel) {
            fIrqLevel = false;
            fIrq->Set(0);
        }
        return;
    }

    bool level = false;
    for (int i = 0; i < NVME_MAX_QUEUES; i++) {
        NVMeCompQueue *cq = &fCq[i];
        if (cq->enabled && cq->ien && cq->head != cq->tail &&
            (fIntMask & (1u << (cq->vector & 31))) == 0) {
            level = true;
            break;
        }
    }
    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


void NVMeDevice::ProcessSq(int sqid)
{
    NVMeSubQueue *sq = &fSq[sqid];

    while (!fBusy && sq->enabled && sq->head != sq->tail) {
        uint8_t raw[NVME_SQE_SIZE];
        if (!DmaRead(sq->base + (uint64_t)sq->head * NVME_SQE_SIZE, raw,
                     NVME_SQE_SIZE)) {
            fCsts |= CSTS_CFS;
            return;
        }

        NVMeCommand cmd;
        cmd.cdw0 = get_le32(raw);
        cmd.nsid = get_le32(raw + 4);
        cmd.mptr = get_le64(raw + 16);
        cmd.prp1 = get_le64(raw + 24);
        cmd.prp2 = get_le64(raw + 32);
        cmd.cdw10 = get_le32(raw + 40);
        cmd.cdw11 = get_le32(raw + 44);
        cmd.cdw12 = get_le32(raw + 48);
        cmd.cdw13 = get_le32(raw + 52);
        cmd.cdw14 = get_le32(raw + 56);
        cmd.cdw15 = get_le32(raw + 60);

        sq->head++;
        if (sq->head >= sq->size) {
            sq->head = 0;
        }

        Execute(sqid, cmd);
    }
}


//#pragma mark - commands

int NVMeDevice::CmdCreateCq(const NVMeCommand &cmd)
{
    uint16_t qid = cmd.cdw10 & 0xffff;
    uint32_t size = ((cmd.cdw10 >> 16) & 0xffff) + 1;
    bool contiguous = (cmd.cdw11 & 1) != 0;

    if (qid == 0 || qid >= NVME_MAX_QUEUES) {
        return (NVME_SCT_SPECIFIC << 8) | NVME_SC_QID_INVALID;
    }
    if (size < 2 || size > NVME_MAX_QUEUE_ENTRIES) {
        return (NVME_SCT_SPECIFIC << 8) | NVME_SC_QSIZE_INVALID;
    }
    if (!contiguous && (fQuirks & NVME_QUIRK_LOOSE_QUEUE_CREATE) == 0) {
        return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_FIELD;
    }
    uint16_t vector = cmd.cdw11 >> 16;
    if (fMsix.Present() && vector >= fMsix.VectorCount()) {
        return (NVME_SCT_SPECIFIC << 8) | NVME_SC_INVALID_VECTOR;
    }

    NVMeCompQueue *cq = &fCq[qid];
    cq->base = cmd.prp1;
    cq->size = size;
    cq->head = 0;
    cq->tail = 0;
    cq->phase = true;
    cq->ien = (cmd.cdw11 & 2) != 0;
    cq->vector = vector;
    cq->enabled = true;
    return 0;
}


int NVMeDevice::CmdCreateSq(const NVMeCommand &cmd)
{
    uint16_t qid = cmd.cdw10 & 0xffff;
    uint32_t size = ((cmd.cdw10 >> 16) & 0xffff) + 1;
    bool contiguous = (cmd.cdw11 & 1) != 0;
    uint16_t cqid = cmd.cdw11 >> 16;

    if (qid == 0 || qid >= NVME_MAX_QUEUES) {
        return (NVME_SCT_SPECIFIC << 8) | NVME_SC_QID_INVALID;
    }
    if (size < 2 || size > NVME_MAX_QUEUE_ENTRIES) {
        return (NVME_SCT_SPECIFIC << 8) | NVME_SC_QSIZE_INVALID;
    }
    if (!contiguous && (fQuirks & NVME_QUIRK_LOOSE_QUEUE_CREATE) == 0) {
        return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_FIELD;
    }
    if (cqid == 0 && (fQuirks & NVME_QUIRK_LOOSE_QUEUE_CREATE) != 0) {
        /* A driver that creates its submission queue before the completion
           queue it will use leaves this field zero. Pairing the two by
           identifier is what it means, and is what a conformant driver asks
           for explicitly. */
        cqid = qid;
    }
    if (cqid == 0 || cqid >= NVME_MAX_QUEUES || !fCq[cqid].enabled) {
        /* The completion queue must already exist, unless the same leniency
           lets it be created afterwards. */
        if ((fQuirks & NVME_QUIRK_LOOSE_QUEUE_CREATE) == 0) {
            return (NVME_SCT_SPECIFIC << 8) | NVME_SC_CQ_INVALID;
        }
    }

    NVMeSubQueue *sq = &fSq[qid];
    sq->base = cmd.prp1;
    sq->size = size;
    sq->head = 0;
    sq->tail = 0;
    sq->cqid = cqid;
    sq->enabled = true;
    return 0;
}


void NVMeDevice::IdentifyController(uint8_t *buf)
{
    memset(buf, 0, 4096);

    put_le16(buf + 0, 0x1b36);  /* vendor */
    put_le16(buf + 2, 0x1b36);  /* subsystem vendor */
    /* Serial, model and firmware revision are ASCII, space padded and not
       NUL terminated. */
    memset(buf + 4, ' ', 20);
    memcpy(buf + 4, "TEMUNVME0000001", 15);
    memset(buf + 24, ' ', 40);
    memcpy(buf + 24, "TinyEMU NVMe Controller", 23);
    memset(buf + 64, ' ', 8);
    memcpy(buf + 64, "1.0", 3);

    buf[72] = 0;            /* RAB */
    buf[77] = NVME_MDTS;    /* maximum data transfer size */
    put_le16(buf + 78, 1);  /* controller id */
    put_le32(buf + 80, 0x00010400); /* version 1.4.0 */

    put_le16(buf + 256, 0);  /* OACS: no optional admin commands */
    buf[258] = 3;            /* abort command limit */
    buf[259] = NVME_MAX_AERS - 1; /* async event request limit */
    buf[260] = 0;            /* firmware updates */
    buf[261] = 0;            /* log page attributes */
    buf[262] = 0;            /* error log page entries */
    buf[263] = 0;            /* number of power states, 0 based */

    buf[512] = 0x66;         /* submission queue entry size: 64 required and max */
    buf[513] = 0x44;         /* completion queue entry size: 16 */
    put_le16(buf + 514, 0);  /* maximum outstanding commands */
    /* The highest namespace id in use, not the largest this could hold: a
       host walks 1..NN calling Identify Namespace and gives up on the whole
       controller if one of them fails. */
    put_le32(buf + 516, fMaxNsid);
    put_le16(buf + 520, 0);  /* ONCS: no optional NVM commands */
    buf[525] = 0;            /* no volatile write cache */
    put_le32(buf + 536, 0);  /* SGL support: none, so PRPs only */

    /* Linux warns about a missing subsystem qualified name, so give it a
       well formed one. */
    snprintf((char *)buf + 768, 256,
             "nqn.2014-08.org.nvmexpress:1b36:1b36:TEMUNVME0000001");
}


void NVMeDevice::IdentifyNamespace(uint8_t *buf, NVMeNamespace *ns)
{
    uint64_t blocks = ns->BlockCount();

    memset(buf, 0, 4096);
    put_le64(buf + 0, blocks);  /* namespace size */
    put_le64(buf + 8, blocks);  /* capacity */
    put_le64(buf + 16, blocks); /* utilisation */
    buf[24] = 0;                /* features */
    buf[25] = 0;                /* one LBA format, 0 based */
    buf[26] = 0;                /* formatted with LBA format 0 */
    buf[27] = 0;                /* no metadata */
    buf[28] = 0;                /* no end to end protection */
    /* NGUID at 104 and EUI64 at 120 stay zero, matching the empty namespace
       descriptor list: a host reads both and treats a disagreement between
       them as a controller bug. */
    /* LBA format 0: no metadata, 2^9 = 512 byte blocks, best performance. */
    put_le32(buf + 128, 9 << 16);
}


int NVMeDevice::CmdIdentify(const NVMeCommand &cmd)
{
    uint8_t buf[4096];
    uint8_t cns = cmd.cdw10 & 0xff;

    switch (cns) {
    case NVME_CNS_CONTROLLER:
        IdentifyController(buf);
        break;

    case NVME_CNS_NAMESPACE: {
        uint32_t nsid = cmd.nsid;
        if (nsid == 0 && (fQuirks & NVME_QUIRK_NSID_ZERO) != 0) {
            nsid = 1;
        }
        if (nsid < 1 || nsid > NVME_MAX_NAMESPACES) {
            return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_NS;
        }
        if (fNamespaces[nsid] == nullptr) {
            /* An id within the reported count but with nothing attached is
               answered with zeros rather than an error: a host walks 1..NN
               and abandons the whole controller if one of them fails. */
            memset(buf, 0, sizeof(buf));
            break;
        }
        IdentifyNamespace(buf, fNamespaces[nsid]);
        break;
    }

    case NVME_CNS_NS_LIST: {
        /* Every namespace with an id above the one asked for, in order. */
        memset(buf, 0, sizeof(buf));
        int count = 0;
        for (uint32_t i = cmd.nsid + 1; i <= NVME_MAX_NAMESPACES; i++) {
            if (fNamespaces[i] != nullptr && count < 1024) {
                put_le32(buf + count * 4, i);
                count++;
            }
        }
        break;
    }

    case NVME_CNS_NS_DESC: {
        NVMeNamespace *ns = NamespaceFor(cmd.nsid);
        if (ns == nullptr) {
            return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_NS;
        }
        /* An empty list: a zero length first descriptor ends it. Reporting no
           identifier at all is legal, and is what this is -- there is nothing
           here that would still name the same namespace on another machine,
           which is the only thing the identifiers are for. */
        (void)ns;
        memset(buf, 0, sizeof(buf));
        break;
    }

    default:
        return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_FIELD;
    }

    if (!PrpTransfer(cmd.prp1, cmd.prp2, buf, sizeof(buf), true)) {
        return (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
    }
    return 0;
}


int NVMeDevice::CmdGetLogPage(const NVMeCommand &cmd)
{
    /* No log page here has anything to report, but a driver that asks for one
       during probe must get the bytes it asked for rather than an error. */
    uint32_t dwords = ((cmd.cdw10 >> 16) & 0xffff) + 1;
    uint32_t len = dwords * 4;

    if (len > NVME_MAX_TRANSFER) {
        len = NVME_MAX_TRANSFER;
    }
    if (!EnsureBuffer(len)) {
        return (NVME_SCT_GENERIC << 8) | NVME_SC_INTERNAL_ERROR;
    }
    memset(fBuf, 0, len);
    if (!PrpTransfer(cmd.prp1, cmd.prp2, fBuf, len, true)) {
        return (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
    }
    return 0;
}


int NVMeDevice::CmdSetFeatures(const NVMeCommand &cmd, uint32_t *dw0)
{
    uint8_t fid = cmd.cdw10 & 0xff;

    switch (fid) {
    case NVME_FEAT_NUM_QUEUES: {
        /* Both fields are zero based. A controller may allocate fewer than
           were asked for but never more, and once answered the count holds
           until the next reset. */
        uint32_t want_sq = (cmd.cdw11 & 0xffff) + 1;
        uint32_t want_cq = ((cmd.cdw11 >> 16) & 0xffff) + 1;
        if (want_sq > NVME_MAX_IO_QUEUES) {
            want_sq = NVME_MAX_IO_QUEUES;
        }
        if (want_cq > NVME_MAX_IO_QUEUES) {
            want_cq = NVME_MAX_IO_QUEUES;
        }
        fNumSqAllocated = want_sq;
        fNumCqAllocated = want_cq;
        *dw0 = (want_sq - 1) | ((want_cq - 1) << 16);
        return 0;
    }

    case NVME_FEAT_ASYNC_EVENT:
        /* Nothing here ever raises an event, but refusing to configure them
           makes a driver complain about a controller with no AER support. */
        *dw0 = cmd.cdw11;
        return 0;

    default:
        /* A driver probes which features exist by trying them, so refusing
           one it cannot use is the expected answer, not a failure. */
        return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_FIELD;
    }
}


int NVMeDevice::CmdGetFeatures(const NVMeCommand &cmd, uint32_t *dw0)
{
    uint8_t fid = cmd.cdw10 & 0xff;

    switch (fid) {
    case NVME_FEAT_NUM_QUEUES:
        /* A driver may read this before it sets it, so the answer has to be
           the count that would be allocated. */
        *dw0 = (fNumSqAllocated - 1) | ((fNumCqAllocated - 1) << 16);
        return 0;

    default:
        return (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_FIELD;
    }
}


void NVMeDevice::ExecuteAdmin(const NVMeCommand &cmd)
{
    uint32_t dw0 = 0;
    int status = 0;

    switch (cmd.Opcode()) {
    case NVME_ADM_CREATE_CQ:
        status = CmdCreateCq(cmd);
        break;

    case NVME_ADM_CREATE_SQ:
        status = CmdCreateSq(cmd);
        break;

    case NVME_ADM_DELETE_CQ: {
        uint16_t qid = cmd.cdw10 & 0xffff;
        if (qid == 0 || qid >= NVME_MAX_QUEUES || !fCq[qid].enabled) {
            status = (NVME_SCT_SPECIFIC << 8) | NVME_SC_QID_INVALID;
            break;
        }
        /* Every submission queue feeding it has to go first, or its
           completions would have nowhere to land. */
        for (int i = 1; i < NVME_MAX_QUEUES; i++) {
            if (fSq[i].enabled && fSq[i].cqid == qid) {
                status = (NVME_SCT_SPECIFIC << 8) | NVME_SC_QUEUE_DELETION;
                break;
            }
        }
        if (status != 0) {
            break;
        }
        fCq[qid] = NVMeCompQueue();
        break;
    }

    case NVME_ADM_DELETE_SQ: {
        uint16_t qid = cmd.cdw10 & 0xffff;
        if (qid == 0 || qid >= NVME_MAX_QUEUES || !fSq[qid].enabled) {
            status = (NVME_SCT_SPECIFIC << 8) | NVME_SC_QID_INVALID;
            break;
        }
        fSq[qid] = NVMeSubQueue();
        break;
    }

    case NVME_ADM_IDENTIFY:
        status = CmdIdentify(cmd);
        break;

    case NVME_ADM_GET_LOG:
        status = CmdGetLogPage(cmd);
        break;

    case NVME_ADM_SET_FEAT:
        status = CmdSetFeatures(cmd, &dw0);
        break;

    case NVME_ADM_GET_FEAT:
        status = CmdGetFeatures(cmd, &dw0);
        break;

    case NVME_ADM_ABORT:
        /* Nothing is ever queued long enough to abort, and bit 0 set is how
           the controller says it did not abort the command. */
        dw0 = 1;
        break;

    case NVME_ADM_KEEP_ALIVE:
        break;

    case NVME_ADM_ASYNC_EVENT:
        /* Held outstanding: no event is ever generated, so no completion is
           ever posted for it. That is what real hardware does too. */
        if (fAerCount >= NVME_MAX_AERS) {
            status = (NVME_SCT_SPECIFIC << 8) | NVME_SC_AER_LIMIT;
            break;
        }
        fAerCount++;
        return;

    default:
        nvme_debug("unsupported admin opcode %#x\n", cmd.Opcode());
        status = (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_OPCODE;
        break;
    }

    nvme_debug("admin op %#x nsid %u cdw10 %#x -> sct %d sc %#x\n",
               cmd.Opcode(), cmd.nsid, cmd.cdw10, (status >> 8) & 0x7,
               status & 0xff);
    PostCompletion(0, 0, cmd.CommandId(), dw0, (status >> 8) & 0x7,
                   status & 0xff);
}


void NVMeDevice::ExecuteIo(int sqid, const NVMeCommand &cmd)
{
    uint16_t cqid = fSq[sqid].cqid;
    NVMeNamespace *ns = NamespaceFor(cmd.nsid);
    int status = 0;

    if (cmd.Opcode() == NVME_IO_FLUSH && cmd.nsid == 0xffffffff) {
        /* Flush may be addressed to every namespace at once. */
        PostCompletion(sqid, cqid, cmd.CommandId(), 0, NVME_SCT_GENERIC,
                       NVME_SC_SUCCESS);
        return;
    }
    if (ns == nullptr) {
        PostCompletion(sqid, cqid, cmd.CommandId(), 0, NVME_SCT_GENERIC,
                       NVME_SC_INVALID_NS);
        return;
    }

    switch (cmd.Opcode()) {
    case NVME_IO_FLUSH:
        /* Writes reach the back end before their command is answered, so
           there is nothing held back to flush. */
        break;

    case NVME_IO_READ:
    case NVME_IO_WRITE: {
        bool is_read = cmd.Opcode() == NVME_IO_READ;
        uint64_t slba = ((uint64_t)cmd.cdw11 << 32) | cmd.cdw10;
        /* The block count in the command is zero based. */
        uint32_t blocks = (cmd.cdw12 & 0xffff) + 1;
        uint32_t length = blocks * NVMeNamespace::BlockSize();

        uint32_t tail_pad = 0;
        if (slba >= ns->BlockCount()) {
            status = (NVME_SCT_GENERIC << 8) | NVME_SC_LBA_RANGE;
            break;
        }
        if (blocks > ns->BlockCount() - slba) {
            /* Haiku's boot loader asks for two blocks whenever it wants one,
               so reading the last sector of the disk runs off the end. Under
               its quirk the request is clamped to the namespace and the rest
               of the buffer is zeroed, which is what it would have read
               anyway; otherwise this is the error it is. */
            if ((fQuirks & NVME_QUIRK_LOOSE_QUEUE_CREATE) == 0) {
                status = (NVME_SCT_GENERIC << 8) | NVME_SC_LBA_RANGE;
                break;
            }
            uint32_t fit = (uint32_t)(ns->BlockCount() - slba);
            tail_pad = (blocks - fit) * NVMeNamespace::BlockSize();
            blocks = fit;
        }
        if (length > NVME_MAX_TRANSFER || !EnsureBuffer(length)) {
            status = (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_FIELD;
            break;
        }

        if (tail_pad > 0) {
            /* Whatever the clamp above dropped reads back as zeros. */
            memset(fBuf + length - tail_pad, 0, tail_pad);
        }

        int ret;
        if (is_read) {
            ret = ns->Backend()->ReadAsync(slba, fBuf, blocks, &fCompletion);
        } else {
            if (!PrpTransfer(cmd.prp1, cmd.prp2, fBuf, length, false)) {
                status = (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
                break;
            }
            ret = ns->Backend()->WriteAsync(slba, fBuf, blocks, &fCompletion);
        }

        if (ret > 0) {
            /* The back end took it; the completion finishes the command and
               no queue is served until it does. */
            fBusy = true;
            fPendingCid = cmd.CommandId();
            fPendingSqid = sqid;
            fPendingCqid = cqid;
            fPendingIsRead = is_read;
            fPendingPrp1 = cmd.prp1;
            fPendingPrp2 = cmd.prp2;
            fPendingLength = length;
            return;
        }
        if (ret < 0) {
            status = (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
            break;
        }
        if (is_read &&
            !PrpTransfer(cmd.prp1, cmd.prp2, fBuf, length, true)) {
            status = (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
        }
        break;
    }

    default:
        nvme_debug("unsupported I/O opcode %#x\n", cmd.Opcode());
        status = (NVME_SCT_GENERIC << 8) | NVME_SC_INVALID_OPCODE;
        break;
    }

    if (cmd.nsid != 0) {
        /* Namespace 0 is the boot loader, which reads a sector at a time and
           would bury everything else. */
        nvme_debug("io sq %d op %#x nsid %u slba %llu nlb %u prp %#llx/%#llx "
                   "-> sct %d sc %#x\n", sqid, cmd.Opcode(), cmd.nsid,
                   (unsigned long long)(((uint64_t)cmd.cdw11 << 32)
                                        | cmd.cdw10),
                   (cmd.cdw12 & 0xffff) + 1, (unsigned long long)cmd.prp1,
                   (unsigned long long)cmd.prp2, (status >> 8) & 0x7,
                   status & 0xff);
    }
    PostCompletion(sqid, cqid, cmd.CommandId(), 0, (status >> 8) & 0x7,
                   status & 0xff);
}


void NVMeDevice::Execute(int sqid, const NVMeCommand &cmd)
{
    if (sqid == 0) {
        ExecuteAdmin(cmd);
    } else {
        ExecuteIo(sqid, cmd);
    }
}


void NVMeDevice::BlockDone(int ret)
{
    if (!fBusy) {
        return;
    }
    fBusy = false;

    int status = 0;
    if (ret < 0) {
        status = (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
    } else if (fPendingIsRead &&
               !PrpTransfer(fPendingPrp1, fPendingPrp2, fBuf, fPendingLength,
                            true)) {
        status = (NVME_SCT_GENERIC << 8) | NVME_SC_DATA_XFER_ERROR;
    }

    PostCompletion(fPendingSqid, fPendingCqid, fPendingCid, 0,
                   (status >> 8) & 0x7, status & 0xff);

    /* Whatever arrived while the back end had the command. */
    for (int i = 0; i < NVME_MAX_QUEUES; i++) {
        ProcessSq(i);
    }
}


//#pragma mark - controller state

void NVMeDevice::ControllerEnable()
{
    /* A configuration this cannot honour is reported as a controller failure
       rather than acted on: the alternative is reading queues at the wrong
       stride and scribbling over guest memory. */
    uint32_t mps = (fCc >> CC_MPS_SHIFT) & CC_MPS_MASK;
    uint32_t css = (fCc >> CC_CSS_SHIFT) & 0x7;
    uint32_t iosqes = (fCc >> CC_IOSQES_SHIFT) & 0xf;
    uint32_t iocqes = (fCc >> CC_IOCQES_SHIFT) & 0xf;

    if (mps > 4 || css != 0 || iosqes != 6 || iocqes != 4) {
        vm_error("nvme: unsupported controller configuration %#x\n", fCc);
        fCsts |= CSTS_CFS;
        return;
    }

    ArmAdminQueue();
    fCsts |= CSTS_RDY;
    /* A shutdown reported earlier belongs to the controller that has just
       been replaced, so its state does not carry over. */
    fCsts &= ~(CSTS_CFS | CSTS_SHST_MASK);
}


void NVMeDevice::ControllerDisable()
{
    for (int i = 0; i < NVME_MAX_QUEUES; i++) {
        fSq[i] = NVMeSubQueue();
        fCq[i] = NVMeCompQueue();
    }
    fAerCount = 0;
    fBusy = false;
    fCsts &= ~(CSTS_RDY | CSTS_SHST_MASK);
    fIntMask = 0;
    if (fIrq != nullptr) {
        fIrq->Set(0);
    }
    fIrqLevel = false;
}


//#pragma mark - register file

uint32_t NVMeDevice::ReadDword(uint32_t offset)
{
    if (offset >= NVME_MSIX_TABLE_OFFSET) {
        if (offset < NVME_MSIX_PBA_OFFSET) {
            return fMsix.TableRead(offset - NVME_MSIX_TABLE_OFFSET, 2);
        }
        return fMsix.PbaRead(offset - NVME_MSIX_PBA_OFFSET, 2);
    }
    if (offset >= NVME_DOORBELL_OFFSET) {
        return 0; /* the doorbells read as zero */
    }

    switch (offset) {
    case NVME_CAP_LO:
        /* Maximum queue entries (zero based), contiguous queues required,
           and a ten second timeout in 500 ms units. */
        return (NVME_MAX_QUEUE_ENTRIES - 1) | (1u << 16) | (20u << 24);

    case NVME_CAP_HI:
        /* Doorbell stride 0 (four bytes apart), the NVM command set, and a
           page size range PageSize() can actually honour. */
        return (1u << 5) | (4u << 20);

    case NVME_VS:
        return 0x00010400; /* 1.4.0 */

    case NVME_INTMS:
    case NVME_INTMC:
        return fIntMask;

    case NVME_CC:
        return fCc;

    case NVME_CSTS:
        return fCsts;

    case NVME_AQA:
        return fAqa;

    case NVME_ASQ_LO:
        return (uint32_t)fAsq;
    case NVME_ASQ_HI:
        return (uint32_t)(fAsq >> 32);
    case NVME_ACQ_LO:
        return (uint32_t)fAcq;
    case NVME_ACQ_HI:
        return (uint32_t)(fAcq >> 32);
    }
    return 0;
}


void NVMeDevice::DoorbellWrite(uint32_t offset, uint32_t val)
{
    /* Doorbell stride is zero, so they are four bytes apart: submission queue
       y at 2y and completion queue y at 2y + 1. */
    uint32_t index = offset / 4;
    int qid = index / 2;

    if (qid >= NVME_MAX_QUEUES) {
        return;
    }

    if ((index & 1) == 0) {
        NVMeSubQueue *sq = &fSq[qid];
        if (!sq->enabled || val >= sq->size) {
            return;
        }
        sq->tail = val;
        if (qid == 0 && !AdminReady()) {
            return;
        }
        ProcessSq(qid);
    } else {
        NVMeCompQueue *cq = &fCq[qid];
        if (!cq->enabled || val >= cq->size) {
            return;
        }
        cq->head = val;
        UpdateIrq();
    }
}


void NVMeDevice::WriteDword(uint32_t offset, uint32_t val)
{
    if (offset >= NVME_MSIX_TABLE_OFFSET) {
        if (offset < NVME_MSIX_PBA_OFFSET) {
            fMsix.TableWrite(offset - NVME_MSIX_TABLE_OFFSET, val, 2);
        }
        return;
    }
    if (offset >= NVME_DOORBELL_OFFSET) {
        DoorbellWrite(offset - NVME_DOORBELL_OFFSET, val);
        return;
    }

    switch (offset) {
    case NVME_INTMS:
        fIntMask |= val;
        UpdateIrq();
        return;

    case NVME_INTMC:
        fIntMask &= ~val;
        UpdateIrq();
        return;

    case NVME_CC: {
        bool was_enabled = (fCc & CC_EN) != 0;
        bool enabled = (val & CC_EN) != 0;
        fCc = val;
        if (enabled && !was_enabled) {
            ControllerEnable();
        } else if (!enabled && was_enabled) {
            ControllerDisable();
        }
        /* A shutdown request completes at once; there is nothing here whose
           state has to reach media first. */
        if (((val >> CC_SHN_SHIFT) & CC_SHN_MASK) != 0) {
            fCsts = (fCsts & ~CSTS_SHST_MASK) | (2u << CSTS_SHST_SHIFT);
        }
        return;
    }

    case NVME_AQA:
        fAqa = val;
        if (AdminReady()) {
            ArmAdminQueue();
        }
        return;

    case NVME_ASQ_LO:
        fAsqLo = val;
        return;

    case NVME_ASQ_HI:
        fAsq = ((uint64_t)val << 32) | fAsqLo;
        if (AdminReady()) {
            ArmAdminQueue();
        }
        return;

    case NVME_ACQ_LO:
        fAcqLo = val;
        return;

    case NVME_ACQ_HI:
        fAcq = ((uint64_t)val << 32) | fAcqLo;
        if (AdminReady()) {
            ArmAdminQueue();
        }
        return;
    }
}


uint32_t NVMeDevice::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val = ReadDword(offset & ~3u);

    if (size_log2 >= 2) {
        return val;
    }
    return (val >> ((offset & 3) * 8)) & ((1u << (8 << size_log2)) - 1);
}


void NVMeDevice::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (size_log2 >= 2) {
        WriteDword(offset & ~3u, val);
        return;
    }
    int shift = (offset & 3) * 8;
    uint32_t mask = ((1u << (8 << size_log2)) - 1) << shift;
    uint32_t cur = ReadDword(offset & ~3u);
    WriteDword(offset & ~3u, (cur & ~mask) | ((val << shift) & mask));
}


void NVMeDevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    (void)bar_num;
    fMemRange->SetAddr(addr, enabled);
}


//#pragma mark - lifecycle

bool NVMeDevice::Prepare()
{
    if (ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    /* The guest places the BAR, so no resources are declared. */
    fChildBus = std::make_unique<NVMeBus>(this, this);
    return true;
}


bool NVMeDevice::Realize()
{
    PCIBus *bus = ParentBus()->AsPCIBus();

    fPciDev = pci_register_device(bus, "nvme", -1, 0x1b36, 0x0010, 0x02,
                                  0x0108);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI device\n", Name());
        return false;
    }
    /* The programming interface byte is what identifies this as NVMe rather
       than some other non-volatile memory controller, and it is what the
       guest matches on. */
    pci_device_set_config8(fPciDev, PCI_CLASS_PROG, 0x02);
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    fMsix.Init(fPciDev, 0, NVME_MSIX_VECTORS, NVME_MSIX_TABLE_OFFSET,
               NVME_MSIX_PBA_OFFSET);

    fIrq = pci_device_get_irq(fPciDev, 0);
    PhysMemoryMap *mem_map = pci_device_get_mem_map(fPciDev);
    fMemRange = mem_map->RegisterDevice(0, NVME_BAR_SIZE, this,
                                        DEVIO_SIZE8 | DEVIO_SIZE16 |
                                            DEVIO_SIZE32 | DEVIO_DISABLED);
    /* The specification defines BAR 0 as a 64 bit register pair, so it takes
       the slot after it too. */
    pci_register_bar(fPciDev, 0, NVME_BAR_SIZE,
                     PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_MEM_TYPE_64,
                     this);
    return true;
}


//#pragma mark - factories

uint32_t nvme_quirks_from_name(const char *name)
{
    if (strcmp(name, "no-enable-check") == 0) {
        return NVME_QUIRK_NO_ENABLE_CHECK;
    }
    if (strcmp(name, "nsid-zero") == 0) {
        return NVME_QUIRK_NSID_ZERO;
    }
    if (strcmp(name, "loose-queue-create") == 0) {
        return NVME_QUIRK_LOOSE_QUEUE_CREATE;
    }
    if (strcmp(name, "poll-only") == 0) {
        return NVME_QUIRK_POLL_ONLY;
    }
    if (strcmp(name, "haiku") == 0 || strcmp(name, "haiku-boot-loader") == 0) {
        /* Everything Haiku's stack needs: the first three for its RISC-V boot
           loader's minimal driver, and polling for its kernel driver, whose
           interrupt handler does not touch the completion queue. */
        return NVME_QUIRK_NO_ENABLE_CHECK | NVME_QUIRK_NSID_ZERO |
               NVME_QUIRK_LOOSE_QUEUE_CREATE | NVME_QUIRK_POLL_ONLY;
    }
    return 0;
}


Device *nvme_node_create(const char *name, uint32_t quirks)
{
    return new NVMeDevice(name, quirks);
}


Device *nvme_namespace_node_create(std::unique_ptr<BlockDevice> bs, int nsid)
{
    return new NVMeNamespaceNode(
        std::make_unique<NVMeNamespace>(std::move(bs)), nsid);
}
