/*
 * Synopsys DesignWare Ethernet QoS (dwmac4) media access controller
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
#pragma once

#include "devices.h"
#include "mdio.h"

/* The register file spans MAC, MTL and DMA blocks; real parts publish a
   window far larger than they decode, and so does the device tree node. */
#define DWMAC_REG_SIZE 0x10000

/* Only the part that decodes to something is shadowed. The DMA block's last
   channel ends at 0x1500, and nothing above that is modelled. */
#define DWMAC_SHADOW_SIZE 0x1200

/* Frames larger than this are dropped rather than split: neither reference
   driver reassembles a receive frame spread over several descriptors. */
#define DWMAC_MAX_FRAME 2048

/* Perfect unicast filter entries, MAC_ADDRESS0..31. */
#define DWMAC_ADDR_COUNT 32

/* What a guest driver matches on. The default names a plain DesignWare core,
   which is what Linux's generic dwmac platform driver binds to. */
#define DWMAC_DEFAULT_COMPATIBLE "snps,dwmac-5.10a"
#define DWMAC_DEFAULT_PHY_MODE "rgmii-id"

/* The rate the emitted clocks report: the gigabit transmit clock, which is
   what a driver expects to find on the one it calls "gtx". */
#define DWMAC_CLOCK_HZ 125000000


/* Deviations from a bare DesignWare core that a guest may need, enabled from
   the configuration file. Each names a behaviour rather than a guest, so that
   a different driver needing one of them does not have to ask for the
   others.

   They exist because Haiku's dwmac driver is written against the StarFive
   JH7110, which wraps the same core in a platform binding: it refuses to
   probe without the clocks that binding names, and it learns the link state
   only from the MAC's in band status rather than from the PHY. */
enum {
    /* Emit the clocks a StarFive style binding names, as fixed clocks, and
       point the node's "clocks" at them. A driver written for such a part
       takes a missing one as a reason not to probe at all. */
    DWMAC_QUIRK_CLOCKS = 1 << 0,
    /* Report the link again after a software reset. The interrupt that
       announces it is a change, and the carrier here comes up before the
       guest has run, so a driver that resets the MAC and then waits for the
       announcement would otherwise wait forever. */
    DWMAC_QUIRK_LINK_ON_RESET = 1 << 1,
};

/* The quirks one configuration name asks for, or 0 when the name is not
   known. "haiku" is an alias for both of the above, which that one driver
   needs together. */
uint32_t dwmac_quirks_from_name(const char *name);


/* A DesignWare Ethernet QoS MAC with one MTL queue and one DMA channel,
   providing the MDIO bus its PHY sits on. Descriptors are moved synchronously:
   a write to a tail pointer drains the transmit ring, and a frame arriving
   from the host back end is placed straight into the current receive
   descriptor. */
class DwmacDevice final: public Device, public EthernetTarget {
private:
    DeviceContext *fCtx;
    const char *fCompatible;
    const char *fPhyMode;
    uint32_t fQuirks;
    /* The core revision the name above selects, settled once the device is
       prepared so that a name the model cannot be is reported then rather
       than confusing a driver later. */
    uint32_t fVersion = 0;

    std::unique_ptr<HostEthernet> fNet;
    std::unique_ptr<MDIOBus> fMdioBus;
    PhysMemoryMap *fMemMap = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;

    Resource *fMmioRes = nullptr;
    Resource *fIrqRes = nullptr;

    /* Registers the guest writes and reads back unchanged. The ones with
       behaviour are intercepted in Read()/Write() and are the only state that
       actually drives the model. */
    uint32_t fRegs[DWMAC_SHADOW_SIZE / 4] {};

    /* Latched separately because MAC_INTERRUPT_STATUS is read only and is
       cleared by reading MAC_PHYIF_CONTROL_STATUS rather than by writing. */
    uint32_t fMacIntStatus = 0;

    uint32_t fTxCur = 0;
    uint32_t fRxCur = 0;

    /* Transmitting hands the frame to the back end, which may hand one back
       before returning; guard against the ring being drained twice over. */
    bool fTxRunning = false;

    void Reset();

    uint32_t &Reg(uint32_t offset) {return fRegs[offset / 4];}
    uint32_t Reg(uint32_t offset) const {return fRegs[offset / 4];}
    uint32_t &ChanReg(uint32_t offset);
    uint32_t ChanReg(uint32_t offset) const;

    MDIODevice *Phy() const;
    uint32_t PhyIfStatus() const;
    void MdioTransfer(uint32_t val);

    void RaiseDma(uint32_t bits);
    void UpdateIrq();

    bool DmaRead(uint64_t addr, uint8_t *buf, int len);
    bool DmaWrite(uint64_t addr, const uint8_t *buf, int len);
    uint64_t BufferAddress(uint32_t lo, uint32_t hi) const;
    uint32_t DescStride() const;
    uint32_t RingLength(bool tx) const;
    uint64_t DescAddress(bool tx, uint32_t index) const;
    bool ReadDesc(uint64_t addr, uint32_t desc[4]);
    bool WriteDesc(uint64_t addr, const uint32_t desc[4]);

    bool TxEnabled() const;
    bool RxEnabled() const;
    bool RxDescAvailable(uint32_t desc[4], uint64_t *addr_out);
    bool AddressMatches(const uint8_t *buf, int len) const;
    void TxPoll();

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);
    /* The whole register, which is the only width anything here decodes. */
    uint32_t ReadWord(uint32_t offset);
    void WriteWord(uint32_t offset, uint32_t val);

public:
    DwmacDevice(DeviceContext *ctx, std::unique_ptr<HostEthernet> net,
                const char *compatible, const char *phy_mode,
                uint32_t quirks);
    ~DwmacDevice() override;

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override;

    /* EthernetTarget */
    bool CanWritePacket() override;
    void WritePacket(const uint8_t *buf, int len) override;
    void SetCarrier(bool carrier_state) override;

    DeviceIOAdapter<DwmacDevice, &DwmacDevice::Read,
                    &DwmacDevice::Write> fIo {*this};
};
