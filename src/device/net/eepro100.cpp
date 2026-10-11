/*
 * Intel 8255x (EtherExpress PRO/100) Ethernet controller
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
#include "eepro100.h"

#include <string.h>

#include "bits.h"
#include "config_props.h"
#include "cutils.h"
#include "mdio.h"

//#define DEBUG_EEPRO100

#ifdef DEBUG_EEPRO100
#include <stdio.h>
#define ee_trace(...) fprintf(stderr, __VA_ARGS__)
#else
#define ee_trace(...) do {} while (0)
#endif

/* The 82558 B-step, and the class code of an Ethernet controller. */
#define EEPRO100_PCI_VENDOR_ID 0x8086
#define EEPRO100_PCI_DEVICE_ID 0x1229
#define EEPRO100_PCI_REVISION  0x05
#define EEPRO100_PCI_CLASS     0x0200

/* EEPROM words drivers read besides the station address. */
#define EEPROM_COMPAT  0x03 /* no receive lock-up workaround needed */
#define EEPROM_CNTR    0x05 /* controller type in the high byte */
#define EEPROM_PRI_PHY 0x06 /* PHY type in bits 13:8, address in 4:0 */

/* The serial transfer: waiting for the start bit, taking the opcode and
   address, then moving data. */
enum {
    EE_STATE_START,
    EE_STATE_COMMAND,
    EE_STATE_READ,
    EE_STATE_WRITE,
    EE_STATE_WRITE_ALL,
    EE_STATE_DONE,
};

/* A list that never suspends or ends is a driver bug; it is cut off rather
   than looped on forever. */
#define EEPRO100_MAX_COMMANDS 4096
#define EEPRO100_MAX_TBDS     256

/* The configuration a reset leaves. */
static const uint8_t kDefaultConfig[EE_CONFIG_BYTES] = {
    0x16, 0x08, 0x00, 0x00, 0x00, 0x00, 0x32, 0x03,
    0x01, 0x00, 0x2e, 0x00, 0x60, 0x00, 0xf2, 0x48,
    0x00, 0x40, 0xf2, 0x80, 0x3f, 0x05,
};


//#pragma mark - construction

EEPro100Device::EEPro100Device(DeviceContext *ctx,
                               std::unique_ptr<HostEthernet> net,
                               const char *name):
    Device(name),
    fCtx(ctx),
    fNet(std::move(net))
{
}


EEPro100Device::~EEPro100Device() = default;


bool EEPro100Device::Prepare()
{
    if (ParentBus() == nullptr || ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    return true;
}


bool EEPro100Device::Realize()
{
    PCIBus *bus = ParentBus()->AsPCIBus();
    int devio_flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32 |
        DEVIO_DISABLED;

    if (fNet == nullptr) {
        vm_error("%s: no network back end\n", Name());
        return false;
    }

    fPciDev = pci_register_device(bus, Name(), -1, EEPRO100_PCI_VENDOR_ID,
                                  EEPRO100_PCI_DEVICE_ID,
                                  EEPRO100_PCI_REVISION, EEPRO100_PCI_CLASS);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI function\n", Name());
        return false;
    }
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    PhysMemoryMap *port_map = pci_device_get_port_map(fPciDev);
    if (port_map == nullptr) {
        vm_error("%s: this PCI bus has no I/O port space\n", Name());
        return false;
    }
    fMemRange = pci_device_get_mem_map(fPciDev)->RegisterDevice(
        0, EEPRO100_MEM_BAR_SIZE, &fIo, devio_flags);
    pci_register_bar(fPciDev, 0, EEPRO100_MEM_BAR_SIZE,
                     PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_MEM_PREFETCH,
                     this);
    fIoRange = port_map->RegisterDevice(0, EEPRO100_IO_BAR_SIZE, &fIo,
                                        devio_flags);
    pci_register_bar(fPciDev, 1, EEPRO100_IO_BAR_SIZE, PCI_ADDRESS_SPACE_IO,
                     this);
    fIrq = pci_device_get_irq(fPciDev, 0);

    LoadEeprom();
    SoftwareReset();
    fNet->target = this;
    fCtx->ethernet.push_back(fNet.get());
    return true;
}


/* What an adapter's EEPROM holds: the station address, an 82558 with its
   82555 PHY at address 1, and the checksum that makes the words add up. */
void EEPro100Device::LoadEeprom()
{
    const uint8_t *mac = fNet->mac_addr;
    uint16_t sum = 0;

    memset(fEeprom, 0, sizeof(fEeprom));
    for (int i = 0; i < 3; i++) {
        fEeprom[i] = mac[i * 2] | (mac[i * 2 + 1] << 8);
    }
    fEeprom[EEPROM_COMPAT] = 0x0003;
    fEeprom[EEPROM_CNTR] = 0x0201;
    fEeprom[EEPROM_PRI_PHY] = 0x0700 | EEPRO100_PHY_ADDR;
    for (int i = 0; i < EEPRO100_EEPROM_WORDS - 1; i++) {
        sum += fEeprom[i];
    }
    fEeprom[EEPRO100_EEPROM_WORDS - 1] = EEPRO100_EEPROM_SUM - sum;
}


/* Everything but PCI configuration and the EEPROM goes back to how power on
   left it. */
void EEPro100Device::SoftwareReset()
{
    SelectiveReset();
    memset(fCsr, 0, sizeof(fCsr));
    fStatAck = 0;
    fIntCtl = 0;
    fPointer = 0;
    /* An idle MDI reads ready, which drivers wait for before a cycle. */
    fMdi = EE_MDI_READY;
    fCuBase = 0;
    fRuBase = 0;
    fDumpAddr = 0;
    memset(fStats, 0, sizeof(fStats));
    memcpy(fConfig, kDefaultConfig, sizeof(fConfig));
    memset(fIa, 0xff, sizeof(fIa));
    fMulticast.clear();
    fEeCtl = 0;
    fEeDo = true;
    fEeState = EE_STATE_START;
    fPhyBmcr = MII_BMCR_ANEG_ENABLE | MII_BMCR_SPEED100;
    fPhyAnar = 0x01e1;
    UpdateIrq();
}


/* Both units go idle; the configuration, the addresses, the bases and the
   counters stay. */
void EEPro100Device::SelectiveReset()
{
    fCuState = EE_CU_IDLE;
    fCuNext = 0;
    fCuLast = 0;
    fRuState = EE_RU_IDLE;
    fRuNext = 0;
}


void EEPro100Device::UpdateIrq()
{
    uint8_t pending = fStatAck;

    /* The masks sit in the interrupt control byte in a different order from
       the status bits they hide. */
    if (fIntCtl & EE_INT_MASK_CX) {
        pending &= ~EE_STAT_CX;
    }
    if (fIntCtl & EE_INT_MASK_FR) {
        pending &= ~EE_STAT_FR;
    }
    if (fIntCtl & EE_INT_MASK_CNA) {
        pending &= ~EE_STAT_CNA;
    }
    if (fIntCtl & EE_INT_MASK_RNR) {
        pending &= ~EE_STAT_RNR;
    }
    if (fIntCtl & EE_INT_MASK_ER) {
        pending &= ~EE_STAT_ER;
    }
    if (fIntCtl & EE_INT_MASK_FCP) {
        pending &= ~EE_STAT_FCP;
    }
    bool level = pending != 0 && (fIntCtl & EE_INT_M) == 0;

    if (fPciDev != nullptr &&
        (pci_device_get_config(fPciDev, PCI_COMMAND, 1) &
         PCI_COMMAND_INTX_DISABLE) != 0) {
        level = false;
    }
    if (fIrq != nullptr && level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


void EEPro100Device::Interrupt(uint8_t stat)
{
    fStatAck |= stat;
    UpdateIrq();
}


//#pragma mark - shared memory

bool EEPro100Device::DmaRead(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *dst = static_cast<uint8_t *>(buf);
    uint64_t a = addr;

    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (a & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, a, false);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(dst, ptr, l);
        a += l;
        dst += l;
        len -= l;
    }
    return true;
}


bool EEPro100Device::DmaWrite(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = static_cast<const uint8_t *>(buf);
    uint64_t a = addr;

    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (a & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, a, true);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(ptr, src, l);
        a += l;
        src += l;
        len -= l;
    }
    return true;
}


/* All ones where nothing is mapped, which ends any list it is a link of. */
uint32_t EEPro100Device::DmaRead32(uint32_t addr)
{
    uint8_t buf[4];

    if (!DmaRead(addr, buf, sizeof(buf))) {
        return 0xffffffff;
    }
    return get_le32(buf);
}


void EEPro100Device::DmaWrite16(uint32_t addr, uint16_t val)
{
    uint8_t buf[2];

    put_le16(buf, val);
    DmaWrite(addr, buf, sizeof(buf));
}


//#pragma mark - the command unit

void EEPro100Device::Command(uint8_t cmd)
{
    int cuc = get_bits(cmd, 4, 4);
    int ruc = get_bits(cmd, 0, 3);

    if (cuc != 0) {
        CuCommand(cuc);
    }
    if (ruc != 0) {
        RuCommand(ruc);
    }
}


void EEPro100Device::CuCommand(int cuc)
{
    switch (cuc) {
    case EE_CUC_START:
    case EE_CUC_HPQ_START:
        RunCommandList(fCuBase + fPointer);
        break;
    case EE_CUC_RESUME:
    case EE_CUC_STATIC_RESUME:
    case EE_CUC_HPQ_RESUME:
        /* The block the CU suspended after may have lost its S bit since,
           which is what lets it go on. */
        if (fCuState == EE_CU_SUSPENDED &&
            (get_bits(DmaRead32(fCuLast), 16, 16) & EE_CB_S) == 0) {
            RunCommandList(fCuBase + fCuNext);
        }
        break;
    case EE_CUC_DUMP_ADDR:
        fDumpAddr = fPointer;
        break;
    case EE_CUC_DUMP:
        DumpCounters(false);
        break;
    case EE_CUC_BASE:
        fCuBase = fPointer;
        break;
    case EE_CUC_DUMP_RESET:
        DumpCounters(true);
        break;
    }
}


void EEPro100Device::RunCommandList(uint32_t addr)
{
    fCuState = EE_CU_ACTIVE;
    for (int n = 0; n < EEPRO100_MAX_COMMANDS; n++) {
        uint8_t cb[8];

        if (!DmaRead(addr, cb, sizeof(cb))) {
            break;
        }
        uint16_t command = get_le16(cb + 2);
        uint32_t link = get_le32(cb + 4);
        uint16_t status = EE_CB_OK;

        ee_trace("eepro100: CB %08x command %04x link %08x\n", addr, command,
                 link);

        if (!ExecuteCommand(addr, command, &status)) {
            break;
        }
        DmaWrite16(addr, EE_CB_C | status);
        if ((command & EE_CB_I) != 0) {
            Interrupt(EE_STAT_CX);
        }
        fCuLast = addr;
        fCuNext = link;
        if ((command & EE_CB_EL) != 0) {
            fCuState = EE_CU_IDLE;
            Interrupt(EE_STAT_CNA);
            break;
        }
        if ((command & EE_CB_S) != 0) {
            fCuState = EE_CU_SUSPENDED;
            if ((fConfig[6] & EE_CFG6_CI) == 0) {
                Interrupt(EE_STAT_CNA);
            }
            break;
        }
        addr = fCuBase + link;
    }
    /* A block the CU cannot reach, or a list with no end. */
    if (fCuState == EE_CU_ACTIVE) {
        fCuState = EE_CU_IDLE;
        Interrupt(EE_STAT_CNA);
    }
    fNet->TargetReady();
}


/* False when the block's parameters are not in memory. */
bool EEPro100Device::ExecuteCommand(uint32_t addr, uint16_t command,
                                    uint16_t *status)
{
    switch (command & EE_CB_CMD_MASK) {
    case EE_CMD_NOP:
    case EE_CMD_MICROCODE:
    case EE_CMD_DIAGNOSE:
        return true;
    case EE_CMD_IA_SETUP:
        return DmaRead(addr + 8, fIa, sizeof(fIa));
    case EE_CMD_CONFIGURE: {
        uint8_t bytes[EE_CONFIG_BYTES];
        int count = get_bits(DmaRead32(addr + 8), 0, 6);

        if (count < 8) {
            count = 8;
        } else if (count > EE_CONFIG_BYTES) {
            count = EE_CONFIG_BYTES;
        }
        if (!DmaRead(addr + 8, bytes, count)) {
            return false;
        }
        memcpy(fConfig, bytes, count);
        /* Promiscuous mode or a filter change can admit what the back end
           holds. */
        fNet->TargetReady();
        return true;
    }
    case EE_CMD_MULTICAST: {
        uint8_t buf[2];

        if (!DmaRead(addr + 8, buf, sizeof(buf))) {
            return false;
        }
        int count = (get_le16(buf) & 0x3fff) / 6;
        fMulticast.clear();
        for (int i = 0; i < count; i++) {
            std::array<uint8_t, 6> mac;
            if (!DmaRead(addr + 10 + i * 6, mac.data(), mac.size())) {
                return false;
            }
            fMulticast.push_back(mac);
        }
        fNet->TargetReady();
        return true;
    }
    case EE_CMD_TRANSMIT:
        Transmit(addr, command);
        return true;
    case EE_CMD_DUMP: {
        uint8_t zero[EE_DUMP_DWORDS * 4] = {};
        uint32_t buffer = DmaRead32(addr + 8);

        DmaWrite(fCuBase + buffer, zero, sizeof(zero));
        return true;
    }
    }
    *status = 0;
    return true;
}


/* Gathers a frame from the TxCB and its TBDs and hands it to the back end.
   The C bit means the data has been read, so a frame that cannot be gathered
   still completes. */
void EEPro100Device::Transmit(uint32_t addr, uint16_t command)
{
    uint8_t tcb[8];
    uint32_t len = 0;

    if (!DmaRead(addr + 8, tcb, sizeof(tcb))) {
        return;
    }
    uint32_t tbd_array = get_le32(tcb);
    uint32_t tcb_count = get_le16(tcb + 4) & EE_TBD_SIZE_MASK;
    int tbd_count = tcb[7];
    /* An extended TxCB is eight dwords: the first two TBDs follow the
       standard four, and the array holds the third onwards. */
    bool extended = (fConfig[6] & EE_CFG6_STD_TXCB) == 0;
    bool dynamic = (fConfig[7] & EE_CFG7_DYN_TBD) != 0;

    auto append = [&](uint32_t from, uint32_t count) {
        if (count > EEPRO100_MAX_FRAME - len) {
            count = EEPRO100_MAX_FRAME - len;
        }
        if (count > 0 && DmaRead(from, fFrame + len, count)) {
            len += count;
        }
    };

    if ((command & EE_CB_SF) == 0) {
        append(addr + 16, tcb_count);
    } else {
        if (tcb_count > 0) {
            append(addr + (extended ? 32 : 16), tcb_count);
        }
        if (dynamic && tbd_count == 0xff) {
            tbd_count = EEPRO100_MAX_TBDS;
        }
        for (int i = 0; i < tbd_count; i++) {
            uint32_t tbd;
            if (!extended) {
                tbd = tbd_array + i * 8;
            } else if (i < 2) {
                tbd = addr + 16 + i * 8;
            } else {
                tbd = tbd_array + (i - 2) * 8;
            }
            uint32_t buf = DmaRead32(tbd);
            uint32_t size = DmaRead32(tbd + 4);
            if (dynamic && buf == 0) {
                break;
            }
            append(buf, size & EE_TBD_SIZE_MASK);
            if ((size & EE_TBD_EL) != 0) {
                break;
            }
        }
    }

    if (len < 14) {
        return;
    }
    if ((command & EE_CB_NC) == 0 && (fConfig[10] & EE_CFG10_NSAI) == 0) {
        memcpy(fFrame + 6, fIa, sizeof(fIa));
    }
    if ((fConfig[18] & EE_CFG18_PADDING) != 0 && len < EEPRO100_MIN_FRAME) {
        memset(fFrame + len, 0x7e, EEPRO100_MIN_FRAME - len);
        len = EEPRO100_MIN_FRAME;
    }
    fStats[EE_STAT_TX_GOOD]++;
    if (get_bits(fConfig[10], EE_CFG10_LOOPBACK_SHIFT, 2) != 0) {
        ReceiveFrame(fFrame, len);
        return;
    }
    fNet->WritePacket(fFrame, len);
}


/* The counters, then a word that tells the driver the dump is complete. */
void EEPro100Device::DumpCounters(bool reset)
{
    uint8_t buf[(EE_STATS_EXT + 1) * 4];
    int count = (fConfig[6] & EE_CFG6_STD_STATS) != 0 ? EE_STATS_STD :
        EE_STATS_EXT;

    for (int i = 0; i < count; i++) {
        put_le32(buf + i * 4, fStats[i]);
    }
    put_le32(buf + count * 4, reset ? 0xa007 : 0xa005);
    DmaWrite(fDumpAddr, buf, (count + 1) * 4);
    if (reset) {
        memset(fStats, 0, sizeof(fStats));
    }
}


void EEPro100Device::Port(uint32_t val)
{
    uint32_t addr = val & ~(uint32_t)0xf;

    switch (val & 0xf) {
    case EE_PORT_RESET:
        SoftwareReset();
        break;
    case EE_PORT_SELFTEST: {
        /* The ROM signature, which is never zero so that a driver can tell
           it was written, then the results: every test passed. */
        uint8_t result[8] = {};
        put_le32(result, EE_SELFTEST_SIGNATURE);
        SelectiveReset();
        DmaWrite(addr, result, sizeof(result));
        SoftwareReset();
        break;
    }
    case EE_PORT_SELECTIVE_RESET:
        SelectiveReset();
        break;
    case EE_PORT_DUMP: {
        uint8_t dump[EE_DUMP_DWORDS * 4] = {};
        put_le32(dump + (EE_DUMP_DWORDS - 1) * 4, 0xa006);
        DmaWrite(addr, dump, sizeof(dump));
        break;
    }
    }
}


//#pragma mark - the receive unit

void EEPro100Device::RuCommand(int ruc)
{
    switch (ruc) {
    case EE_RUC_START:
        fRuNext = fPointer;
        fRuState = EE_RU_READY;
        fNet->TargetReady();
        break;
    case EE_RUC_RESUME:
        if (fRuState == EE_RU_SUSPENDED) {
            fRuState = EE_RU_READY;
            fNet->TargetReady();
        }
        break;
    case EE_RUC_ABORT:
        if (fRuState == EE_RU_READY) {
            Interrupt(EE_STAT_RNR);
        }
        fRuState = EE_RU_IDLE;
        break;
    case EE_RUC_BASE:
        fRuBase = fPointer;
        break;
    }
}


/* Whether the filter takes a frame, and the status bits that say why. The
   multicast filter is the list itself rather than its hash: a driver never
   depends on what a hash lets through by accident. */
bool EEPro100Device::AddressMatches(const uint8_t *buf, uint16_t *status) const
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    bool promisc = (fConfig[15] & EE_CFG15_PROMISC) != 0;
    bool in_list = false;

    for (const auto &mac: fMulticast) {
        if (memcmp(buf, mac.data(), 6) == 0) {
            in_list = true;
            break;
        }
    }

    *status = 0;
    if (memcmp(buf, fIa, sizeof(fIa)) == 0) {
        return true;
    }
    *status |= EE_RFD_NOT_IA;
    if (memcmp(buf, broadcast, sizeof(broadcast)) == 0) {
        if ((fConfig[15] & EE_CFG15_BCAST_DIS) == 0 || promisc) {
            return true;
        }
    } else if ((buf[0] & 1) != 0) {
        if ((fConfig[21] & EE_CFG21_MC_ALL) != 0 || in_list) {
            return true;
        }
    } else if ((fConfig[20] & EE_CFG20_MULTI_IA) != 0 && in_list) {
        return true;
    }
    *status |= EE_RFD_NO_MATCH;
    return promisc;
}


/* Not while the CU runs: a frame the back end answers a transmit with
   arrives later, as it would on a wire, rather than inside the register
   write that started the transmit. */
bool EEPro100Device::CanWritePacket()
{
    return fRuState == EE_RU_READY && fCuState != EE_CU_ACTIVE;
}


/* A frame the back end sends without asking (an ARP reply, say) is still
   taken while the CU runs. */
void EEPro100Device::WritePacket(const uint8_t *buf, int len)
{
    if (fRuState == EE_RU_READY && len >= 14 && len <= EEPRO100_MAX_FRAME) {
        ReceiveFrame(buf, len);
    }
}


void EEPro100Device::ReceiveFrame(const uint8_t *buf, int len)
{
    uint8_t padded[EEPRO100_MIN_FRAME];
    uint16_t status;

    if (fRuState != EE_RU_READY || !AddressMatches(buf, &status)) {
        return;
    }
    /* The wire pads a short frame out; nothing on the way here does. */
    if (len < EEPRO100_MIN_FRAME) {
        memcpy(padded, buf, len);
        memset(padded + len, 0, EEPRO100_MIN_FRAME - len);
        buf = padded;
        len = EEPRO100_MIN_FRAME;
    }
    uint16_t type_len = (buf[12] << 8) | buf[13];
    if (type_len == 0 || type_len > 1500) {
        status |= EE_RFD_TYPE;
    } else if ((fConfig[18] & EE_CFG18_STRIPPING) != 0 &&
               14 + type_len < len) {
        /* A length field shorter than the frame means padding. */
        len = 14 + type_len;
    }

    uint32_t rfd = fRuBase + fRuNext;
    uint8_t hdr[EE_RFD_HEADER];
    if (!DmaRead(rfd, hdr, sizeof(hdr))) {
        fRuState = EE_RU_NORESOURCES;
        Interrupt(EE_STAT_RNR);
        return;
    }
    uint16_t control = get_le16(hdr + 2);
    uint32_t link = get_le32(hdr + 4);
    uint32_t size = get_le16(hdr + 14) & EE_RFD_COUNT_MASK;


    uint32_t count = (uint32_t)len;
    if (count > size) {
        count = size;
        status |= EE_RFD_NO_RESOURCES;
    } else {
        status |= EE_CB_OK;
    }
    DmaWrite(rfd + EE_RFD_HEADER, buf, count);
    DmaWrite16(rfd + 12, count | EE_RFD_EOF | EE_RFD_F);
    DmaWrite16(rfd, EE_CB_C | status);
    fStats[EE_STAT_RX_GOOD]++;

    uint8_t stat = EE_STAT_FR;
    fRuNext = link;
    if ((control & EE_CB_EL) != 0) {
        fRuState = EE_RU_NORESOURCES;
        stat |= EE_STAT_RNR;
    } else if ((control & EE_CB_S) != 0) {
        fRuState = EE_RU_SUSPENDED;
        stat |= EE_STAT_RNR;
    }
    Interrupt(stat);
}


void EEPro100Device::SetCarrier(bool carrier_state)
{
    fCarrier = carrier_state;
}


//#pragma mark - EEPROM and PHY

/* The guest clocks the 93C46 through the control register: data in is taken
   on each rising edge of the clock while chip select is high, and the
   EEPROM's output changes on the same edge. */
void EEPro100Device::EepromWrite(uint8_t val)
{
    bool cs = (val & EE_EEPROM_CS) != 0;
    bool rising = (val & EE_EEPROM_SK) != 0 && (fEeCtl & EE_EEPROM_SK) == 0;
    int di = (val & EE_EEPROM_DI) != 0;

    if (cs && (fEeCtl & EE_EEPROM_CS) == 0) {
        fEeState = EE_STATE_START;
        fEeDo = true;
    }
    fEeCtl = val & (EE_EEPROM_SK | EE_EEPROM_CS | EE_EEPROM_DI);
    if (!cs) {
        fEeState = EE_STATE_START;
        fEeDo = true;
        return;
    }
    if (!rising) {
        return;
    }

    switch (fEeState) {
    case EE_STATE_START:
        if (di) {
            fEeState = EE_STATE_COMMAND;
            fEeShift = 0;
            fEeBits = 0;
        }
        break;
    case EE_STATE_COMMAND: {
        fEeShift = (fEeShift << 1) | di;
        if (++fEeBits < 2 + EEPRO100_EEPROM_ADDR_BITS) {
            break;
        }
        int op = fEeShift >> EEPRO100_EEPROM_ADDR_BITS;
        fEeAddr = fEeShift & (EEPRO100_EEPROM_WORDS - 1);
        /* The dummy zero that ends the address. */
        fEeDo = false;
        fEeShift = 0;
        fEeBits = 0;
        switch (op) {
        case 2:
            fEeState = EE_STATE_READ;
            fEeShift = fEeprom[fEeAddr];
            break;
        case 1:
            fEeState = EE_STATE_WRITE;
            break;
        case 3:
            if (fEeWriteEnable) {
                fEeprom[fEeAddr] = 0xffff;
            }
            fEeState = EE_STATE_DONE;
            break;
        default:
            switch (fEeAddr >> (EEPRO100_EEPROM_ADDR_BITS - 2)) {
            case 0:
                fEeWriteEnable = false;
                break;
            case 1:
                fEeState = EE_STATE_WRITE_ALL;
                return;
            case 2:
                if (fEeWriteEnable) {
                    memset(fEeprom, 0xff, sizeof(fEeprom));
                }
                break;
            case 3:
                fEeWriteEnable = true;
                break;
            }
            fEeState = EE_STATE_DONE;
            break;
        }
        break;
    }
    case EE_STATE_READ:
        fEeDo = get_bit(fEeShift, 15);
        fEeShift = (fEeShift << 1) & 0xffff;
        /* A read goes on into the next word for as long as it is clocked. */
        if (++fEeBits == 16) {
            fEeAddr = (fEeAddr + 1) & (EEPRO100_EEPROM_WORDS - 1);
            fEeShift = fEeprom[fEeAddr];
            fEeBits = 0;
        }
        break;
    case EE_STATE_WRITE:
    case EE_STATE_WRITE_ALL:
        fEeShift = (fEeShift << 1) | di;
        if (++fEeBits < 16) {
            break;
        }
        if (fEeWriteEnable) {
            if (fEeState == EE_STATE_WRITE) {
                fEeprom[fEeAddr] = fEeShift;
            } else {
                for (auto &word: fEeprom) {
                    word = fEeShift;
                }
            }
        }
        fEeState = EE_STATE_DONE;
        fEeDo = true;
        break;
    case EE_STATE_DONE:
        fEeDo = true;
        break;
    }
}


/* An MDI cycle finishes at once. Only the internal PHY answers. */
void EEPro100Device::MdiCommand(uint32_t val)
{
    int op = get_bits(val, EE_MDI_OP_SHIFT, 2);
    int phy = get_bits(val, EE_MDI_PHY_SHIFT, 5);
    int reg = get_bits(val, EE_MDI_REG_SHIFT, 5);
    uint32_t data = val & EE_MDI_DATA_MASK;

    if (op == EE_MDI_OP_READ) {
        data = phy == EEPRO100_PHY_ADDR ? PhyRead(reg) : MDIO_NO_DEVICE;
    } else if (op == EE_MDI_OP_WRITE && phy == EEPRO100_PHY_ADDR) {
        PhyWrite(reg, data);
    }
    fMdi = (val & ~(uint32_t)EE_MDI_DATA_MASK) | data | EE_MDI_READY;
    if ((val & EE_MDI_IE) != 0) {
        Interrupt(EE_STAT_MDI);
    }
}


/* A 10/100 PHY whose negotiation succeeds at once, at the best mode both
   ends advertise; the link follows the host carrier. */
uint16_t EEPro100Device::PhyRead(int reg)
{
    bool aneg = (fPhyBmcr & MII_BMCR_ANEG_ENABLE) != 0;

    switch (reg) {
    case MII_BMCR:
        return fPhyBmcr;
    case MII_BMSR:
        return 0x7809 | (fCarrier ? (MII_BMSR_LINK | MII_BMSR_ANEG_DONE) : 0);
    case MII_PHYID1:
        return EEPRO100_PHY_ID1;
    case MII_PHYID2:
        return EEPRO100_PHY_ID2;
    case MII_ANAR:
        return fPhyAnar;
    case MII_ANLPAR:
        return aneg ? 0x41e1 : 0;
    case MII_ANER:
        return 0x0001;
    case EE_PHY_SCR: {
        if (!fCarrier) {
            return 0;
        }
        bool s100, fdx;
        if (aneg) {
            s100 = get_bits(fPhyAnar, 7, 2) != 0;
            fdx = s100 ? get_bit(fPhyAnar, 8) : get_bit(fPhyAnar, 6);
        } else {
            s100 = (fPhyBmcr & MII_BMCR_SPEED100) != 0;
            fdx = (fPhyBmcr & MII_BMCR_FULLDUPLEX) != 0;
        }
        return (s100 ? EE_PHY_SCR_100 : 0) | (fdx ? EE_PHY_SCR_FDX : 0);
    }
    default:
        return 0;
    }
}


void EEPro100Device::PhyWrite(int reg, uint16_t val)
{
    switch (reg) {
    case MII_BMCR:
        if ((val & MII_BMCR_RESET) != 0) {
            fPhyBmcr = MII_BMCR_ANEG_ENABLE | MII_BMCR_SPEED100;
            fPhyAnar = 0x01e1;
            break;
        }
        fPhyBmcr = val & ~(MII_BMCR_RESET | MII_BMCR_ANEG_RESTART |
                           MII_BMCR_SPEED1000);
        break;
    case MII_ANAR:
        fPhyAnar = val;
        break;
    }
}


//#pragma mark - the register block

uint8_t EEPro100Device::ReadByte(uint32_t offset)
{
    switch (offset) {
    case EE_SCB_STATUS:
        return (fCuState << 6) | (fRuState << 2);
    case EE_SCB_STATACK:
        return fStatAck;
    case EE_SCB_COMMAND:
        /* Every command has been taken by the time the guest looks. */
        return 0;
    case EE_SCB_INTCTL:
        return fIntCtl;
    case EE_SCB_POINTER:
    case EE_SCB_POINTER + 1:
    case EE_SCB_POINTER + 2:
    case EE_SCB_POINTER + 3:
        return fPointer >> ((offset - EE_SCB_POINTER) * 8);
    case EE_EEPROM:
        return fEeCtl | (fEeDo ? EE_EEPROM_DO : 0);
    case EE_MDI:
    case EE_MDI + 1:
    case EE_MDI + 2:
    case EE_MDI + 3:
        return fMdi >> ((offset - EE_MDI) * 8);
    default:
        /* The 82559's registers above the flow control ones are not
           here. */
        if (offset >= 0x1c) {
            return 0;
        }
        return fCsr[offset];
    }
}


/* A write of 'len' bytes at 'offset'. The SCB bytes take effect in the
   order that matters: acknowledgements and the interrupt control byte before
   the command, so that a dword write that unmasks and commands at once sees
   the new mask. The PORT and MDI registers act once their high byte is
   written. */
void EEPro100Device::WriteBytes(uint32_t offset, uint32_t val, int len)
{
    bool command = false;
    uint8_t command_val = 0;

    for (int i = 0; i < len; i++) {
        uint32_t off = offset + i;
        uint8_t b = val >> (i * 8);

        if (off >= EEPRO100_CSR_SIZE) {
            break;
        }
        fCsr[off] = b;
        switch (off) {
        case EE_SCB_STATACK:
            fStatAck &= ~b;
            break;
        case EE_SCB_COMMAND:
            command = true;
            command_val = b;
            break;
        case EE_SCB_INTCTL:
            fIntCtl = b & ~EE_INT_SI;
            if ((b & EE_INT_SI) != 0) {
                fStatAck |= EE_STAT_SWI;
            }
            break;
        case EE_SCB_POINTER:
        case EE_SCB_POINTER + 1:
        case EE_SCB_POINTER + 2:
        case EE_SCB_POINTER + 3: {
            int shift = (off - EE_SCB_POINTER) * 8;
            fPointer = (fPointer & ~(0xffu << shift)) | ((uint32_t)b << shift);
            break;
        }
        case EE_PORT + 3:
            Port(get_le32(fCsr + EE_PORT));
            break;
        case EE_EEPROM:
            EepromWrite(b);
            break;
        case EE_MDI + 3:
            MdiCommand(get_le32(fCsr + EE_MDI));
            break;
        }
    }
    UpdateIrq();
    if (command) {
        Command(command_val);
    }
}


uint32_t EEPro100Device::Read(uint32_t offset, int size_log2)
{
    int len = 1 << size_log2;
    uint32_t val = 0;

    for (int i = 0; i < len; i++) {
        uint32_t off = offset + i;
        if (off < EEPRO100_CSR_SIZE) {
            val |= (uint32_t)ReadByte(off) << (i * 8);
        }
    }
    if (offset != EE_EEPROM) {
        ee_trace("eepro100: read %02x/%d = %08x\n", offset, len, val);
    }
    return val;
}


void EEPro100Device::Write(uint32_t offset, uint32_t val, int size_log2)
{
    if (offset != EE_EEPROM) {
        ee_trace("eepro100: write %02x/%d = %08x\n", offset,
                 1 << size_log2, val);
    }
    WriteBytes(offset, val, 1 << size_log2);
}


//#pragma mark - PCIBarTarget

void EEPro100Device::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    if (bar_num == 0 && fMemRange != nullptr) {
        fMemRange->SetAddr(addr, enabled);
    } else if (bar_num == 1 && fIoRange != nullptr) {
        fIoRange->SetAddr(addr, enabled);
    }
}


//#pragma mark - class

class EEPro100Class final: public DeviceClass {
public:
    EEPro100Class(): DeviceClass("eepro100") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        auto net = config_open_ethernet(cfg, ctx);
        if (net == nullptr) {
            return nullptr;
        }
        return new EEPro100Device(ctx, std::move(net),
                                  cfg.IdOr("eepro100"));
    }
};

static const EEPro100Class sEEPro100Class;
