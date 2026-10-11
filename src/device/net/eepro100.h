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
#pragma once

#include <array>
#include <vector>

#include "devices.h"
#include "pci.h"

/* The register block (CSR) the two BARs map: 4 KB of memory, of which only
   the first 32 bytes decode, and the same 32 bytes in I/O space. */
#define EEPRO100_MEM_BAR_SIZE 0x1000
#define EEPRO100_IO_BAR_SIZE  0x20
#define EEPRO100_CSR_SIZE     0x20

/* CSR offsets. The SCB is the first eight bytes. */
#define EE_SCB_STATUS  0x00 /* CU and RU state */
#define EE_SCB_STATACK 0x01 /* pending interrupts, write one to acknowledge */
#define EE_SCB_COMMAND 0x02 /* CU and RU commands */
#define EE_SCB_INTCTL  0x03 /* interrupt mask and software interrupt */
#define EE_SCB_POINTER 0x04 /* SCB general pointer */
#define EE_PORT        0x08 /* taken when its high byte is written */
#define EE_EEPROM      0x0e
#define EE_MDI         0x10 /* taken when its high byte is written */

/* SCB status byte: the CU state in bits 7:6, the RU state in bits 5:2. */
#define EE_CU_IDLE      0
#define EE_CU_SUSPENDED 1
#define EE_CU_ACTIVE    2
#define EE_RU_IDLE        0
#define EE_RU_SUSPENDED   1
#define EE_RU_NORESOURCES 2
#define EE_RU_READY       4

/* STAT/ACK byte. */
#define EE_STAT_FCP bit_at(0)
#define EE_STAT_ER  bit_at(1)
#define EE_STAT_SWI bit_at(2)
#define EE_STAT_MDI bit_at(3)
#define EE_STAT_RNR bit_at(4)
#define EE_STAT_CNA bit_at(5)
#define EE_STAT_FR  bit_at(6)
#define EE_STAT_CX  bit_at(7)

/* Interrupt control byte: the global mask, the software interrupt, and one
   mask per maskable source. */
#define EE_INT_M        bit_at(0)
#define EE_INT_SI       bit_at(1)
#define EE_INT_MASK_FCP bit_at(2)
#define EE_INT_MASK_ER  bit_at(3)
#define EE_INT_MASK_RNR bit_at(4)
#define EE_INT_MASK_CNA bit_at(5)
#define EE_INT_MASK_FR  bit_at(6)
#define EE_INT_MASK_CX  bit_at(7)

/* Command byte: the CU command in bits 7:4, the RU command in bits 2:0. */
#define EE_CUC_START        1
#define EE_CUC_RESUME       2
#define EE_CUC_HPQ_START    3
#define EE_CUC_DUMP_ADDR    4
#define EE_CUC_DUMP         5
#define EE_CUC_BASE         6
#define EE_CUC_DUMP_RESET   7
#define EE_CUC_STATIC_RESUME 10
#define EE_CUC_HPQ_RESUME   11
#define EE_RUC_START  1
#define EE_RUC_RESUME 2
#define EE_RUC_ABORT  4
#define EE_RUC_HDS    5
#define EE_RUC_BASE   6

/* PORT opcodes, in bits 3:0; the rest is a 16 byte aligned address. */
#define EE_PORT_RESET           0
#define EE_PORT_SELFTEST        1
#define EE_PORT_SELECTIVE_RESET 2
#define EE_PORT_DUMP            3
/* Any non-zero ROM signature: drivers only check that one was written. */
#define EE_SELFTEST_SIGNATURE   0x0000d4ec

/* The EEPROM control register's low byte, which drivers drive bit by bit. */
#define EE_EEPROM_SK bit_at(0)
#define EE_EEPROM_CS bit_at(1)
#define EE_EEPROM_DI bit_at(2)
#define EE_EEPROM_DO bit_at(3)

/* A 93C46: 64 words, 6 address bits. */
#define EEPRO100_EEPROM_WORDS     64
#define EEPRO100_EEPROM_ADDR_BITS 6
#define EEPRO100_EEPROM_SUM       0xbaba

/* MDI control register. */
#define EE_MDI_DATA_MASK  0xffff
#define EE_MDI_REG_SHIFT  16
#define EE_MDI_PHY_SHIFT  21
#define EE_MDI_OP_SHIFT   26
#define EE_MDI_OP_WRITE   1
#define EE_MDI_OP_READ    2
#define EE_MDI_READY      bit_at(28)
#define EE_MDI_IE         bit_at(29)

/* Where the internal PHY answers, and the 82555 identity it reports. */
#define EEPRO100_PHY_ADDR 1
#define EEPRO100_PHY_ID1  0x02a8
#define EEPRO100_PHY_ID2  0x0150
#define EE_PHY_SCR        0x10 /* 82555 status and control */
#define EE_PHY_SCR_100    bit_at(1)
#define EE_PHY_SCR_FDX    bit_at(0)

/* Every command block and RFD starts with a status word, a command word and
   a link. */
#define EE_CB_C         bit_at(15) /* status: complete */
#define EE_CB_OK        bit_at(13)
#define EE_CB_EL        bit_at(15) /* command: end of list */
#define EE_CB_S         bit_at(14) /* command: suspend after */
#define EE_CB_I         bit_at(13) /* command: interrupt after */
#define EE_CB_NC        bit_at(4)  /* transmit: no CRC or source insertion */
#define EE_CB_SF        bit_at(3)  /* transmit: flexible */
#define EE_CB_CMD_MASK  7
#define EE_CMD_NOP        0
#define EE_CMD_IA_SETUP   1
#define EE_CMD_CONFIGURE  2
#define EE_CMD_MULTICAST  3
#define EE_CMD_TRANSMIT   4
#define EE_CMD_MICROCODE  5
#define EE_CMD_DUMP       6
#define EE_CMD_DIAGNOSE   7

/* A TBD's size word: the byte count and the end of list bit. */
#define EE_TBD_SIZE_MASK 0x3fff
#define EE_TBD_EL        bit_at(15)

/* RFD status bits, and the count word. */
#define EE_RFD_NO_RESOURCES bit_at(9)
#define EE_RFD_TYPE         bit_at(5)
#define EE_RFD_NO_MATCH     bit_at(2)
#define EE_RFD_NOT_IA       bit_at(1)
#define EE_RFD_EOF          bit_at(15)
#define EE_RFD_F            bit_at(14)
#define EE_RFD_COUNT_MASK   0x3fff
#define EE_RFD_HEADER       16

/* Configure bytes. */
#define EE_CONFIG_BYTES 22
#define EE_CFG6_STD_STATS  bit_at(5) /* byte 6: 16 counters, not 19 */
#define EE_CFG6_STD_TXCB   bit_at(4) /* byte 6: 4 dword TxCB, not 8 */
#define EE_CFG6_CI         bit_at(3) /* byte 6: CU idle interrupt only */
#define EE_CFG7_DYN_TBD    bit_at(7) /* byte 7: dynamic TBDs */
#define EE_CFG10_NSAI      bit_at(3) /* byte 10: no source address insertion */
#define EE_CFG10_LOOPBACK_SHIFT 6
#define EE_CFG15_BCAST_DIS bit_at(1)
#define EE_CFG15_PROMISC   bit_at(0)
#define EE_CFG18_PADDING   bit_at(1)
#define EE_CFG18_STRIPPING bit_at(0)
#define EE_CFG20_MULTI_IA  bit_at(6)
#define EE_CFG21_MC_ALL    bit_at(3)

/* The statistical counters: 16, or 19 with the extended ones. */
#define EE_STATS_STD 16
#define EE_STATS_EXT 19
#define EE_STAT_TX_GOOD 0
#define EE_STAT_RX_GOOD 9

/* What a PORT dump fills, with the completion word last. */
#define EE_DUMP_DWORDS 150

#define EEPRO100_MAX_FRAME 2600 /* the most a TxCB byte count describes */
#define EEPRO100_MIN_FRAME 60


/* An 82558 B-step on PCI, with its internal 82555 PHY and a serial EEPROM
 * that holds the station address.
 *
 * The controller works from memory the guest shares with it: the command
 * unit (CU) executes a linked list of command blocks, transmits among them,
 * and the receive unit (RU) fills a linked list of receive frame descriptors
 * (RFDs). Both run synchronously here: a CU start or resume runs the list up
 * to a suspend or end of list, and a frame from the back end goes straight
 * into the next RFD. A frame that arrives while the RU has no RFD stays with
 * the back end until the guest gives it one.
 */
class EEPro100Device final: public Device, public EthernetTarget,
                            public PCIBarTarget {
private:
    DeviceContext *fCtx;
    std::unique_ptr<HostEthernet> fNet;

    PCIDevice *fPciDev = nullptr;
    PhysMemoryRange *fMemRange = nullptr;
    PhysMemoryRange *fIoRange = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;

    /* The CSR bytes that hold plain values; the ones with side effects are
       kept apart below. */
    uint8_t fCsr[EEPRO100_CSR_SIZE] {};

    uint8_t fStatAck = 0;
    uint8_t fIntCtl = 0;
    uint32_t fPointer = 0;
    uint32_t fMdi = 0;

    int fCuState = EE_CU_IDLE;
    uint32_t fCuBase = 0;
    uint32_t fCuNext = 0;  /* link of the block the CU suspended after */
    uint32_t fCuLast = 0;  /* that block's address, for its S bit */
    int fRuState = EE_RU_IDLE;
    uint32_t fRuBase = 0;
    uint32_t fRuNext = 0;  /* the RFD the next frame goes into */
    uint32_t fDumpAddr = 0;
    uint32_t fStats[EE_STATS_EXT] {};

    uint8_t fConfig[EE_CONFIG_BYTES] {};
    uint8_t fIa[6] {};
    std::vector<std::array<uint8_t, 6>> fMulticast;

    /* The EEPROM and the state of the serial transfer the guest drives. */
    uint16_t fEeprom[EEPRO100_EEPROM_WORDS] {};
    uint8_t fEeCtl = 0;
    bool fEeDo = true;
    bool fEeWriteEnable = false;
    int fEeState = 0;
    int fEeBits = 0;
    uint32_t fEeShift = 0;
    int fEeAddr = 0;

    /* The 82555's writable registers, and the host carrier. */
    uint16_t fPhyBmcr = 0;
    uint16_t fPhyAnar = 0;
    bool fCarrier = true;

    uint8_t fFrame[EEPRO100_MAX_FRAME + 4] {};

    void LoadEeprom();
    void SoftwareReset();
    void SelectiveReset();
    void UpdateIrq();
    void Interrupt(uint8_t stat);

    bool DmaRead(uint32_t addr, void *buf, uint32_t len);
    bool DmaWrite(uint32_t addr, const void *buf, uint32_t len);
    uint32_t DmaRead32(uint32_t addr);
    void DmaWrite16(uint32_t addr, uint16_t val);

    void Command(uint8_t cmd);
    void CuCommand(int cuc);
    void RuCommand(int ruc);
    void RunCommandList(uint32_t addr);
    bool ExecuteCommand(uint32_t addr, uint16_t command, uint16_t *status);
    void Transmit(uint32_t addr, uint16_t command);
    void DumpCounters(bool reset);
    void Port(uint32_t val);

    void EepromWrite(uint8_t val);
    void MdiCommand(uint32_t val);
    uint16_t PhyRead(int reg);
    void PhyWrite(int reg, uint16_t val);

    bool AddressMatches(const uint8_t *buf, uint16_t *status) const;
    void ReceiveFrame(const uint8_t *buf, int len);

    uint8_t ReadByte(uint32_t offset);
    void WriteBytes(uint32_t offset, uint32_t val, int len);

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

public:
    EEPro100Device(DeviceContext *ctx, std::unique_ptr<HostEthernet> net,
                   const char *name);
    ~EEPro100Device() override;

    bool Prepare() override;
    bool Realize() override;

    /* EthernetTarget */
    bool CanWritePacket() override;
    void WritePacket(const uint8_t *buf, int len) override;
    void SetCarrier(bool carrier_state) override;

    /* PCIBarTarget */
    void SetBar(int bar_num, uint64_t addr, bool enabled) override;

    DeviceIOAdapter<EEPro100Device, &EEPro100Device::Read,
                    &EEPro100Device::Write> fIo {*this};
};
