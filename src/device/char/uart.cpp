#include "uart.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "acpi.h"
#include "bits.h"
#include "devices.h"
#include "fdt.h"
#include "host_time.h"


SerialState::SerialState(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                         HostConsole *output, uint32_t clock_hz,
                         std::unique_ptr<HostTimer> timer):
    fClockHz(clock_hz),
    fIrq(irq),
    fOutput(output),
    fTimer(std::move(timer))
{
    if (fTimer != nullptr)
        fTimer->target = this;
    port_map->RegisterDevice(addr, 8, &fIo, DEVIO_SIZE8);
}


int SerialState::RxTrigger() const
{
    static const int levels[4] = {1, 4, 8, 14};
    return levels[get_bits(fFcr, 6, 2)];
}


/* The error bits of the characters still in the FIFO, which LSR bit 7
   reports. */
uint8_t SerialState::RxErrorsInFifo() const
{
    uint8_t flags = 0;
    for (int i = 0; i < fRxCount; i++)
        flags |= fRxFlags[(fRxHead + i) % UART_FIFO_LENGTH];
    return flags;
}


void SerialState::UpdateIRQ()
{
    bool data = FifoEnabled() ? fRxCount >= RxTrigger() : fRxCount > 0;

    if ((fLsrErrors & UART_LSR_ERRORS) && (fIer & UART_IER_RLSI)) {
        fIir = UART_IIR_RLSI;
    } else if (data && (fIer & UART_IER_RDI)) {
        fIir = UART_IIR_RDI;
    } else if (fTimeoutPending && (fIer & UART_IER_RDI)) {
        fIir = UART_IIR_CTI;
    } else if (fThrIpending && (fIer & UART_IER_THRI)) {
        fIir = UART_IIR_THRI;
    } else if ((fMsr & UART_MSR_DELTAS) && (fIer & UART_IER_MSI)) {
        fIir = UART_IIR_MSI;
    } else {
        fIir = UART_IIR_NO_INT;
    }
    fIrq->Set(fIir != UART_IIR_NO_INT);
}


/* Start, data, parity and stop bits, each sixteen clocks of the divided
   input clock. */
uint64_t SerialState::CharTimeUs() const
{
    int data_bits = 5 + get_bits(fLcr, 0, 2);
    int half_bits = 2 * (1 + data_bits + ((fLcr & UART_LCR_PARITY) ? 1 : 0));
    if (fLcr & UART_LCR_STOP)
        half_bits += data_bits == 5 ? 3 : 4;
    else
        half_bits += 2;
    uint64_t divisor = fDivider != 0 ? fDivider : 1;
    return (uint64_t)half_bits * 16 * divisor * 1000000 / (2ull * fClockHz);
}


/* Four character times from now, the FIFO reports what is left in it. The
   timer runs while characters wait, and starts again whenever one arrives
   or is read. */
void SerialState::RestartTimeout()
{
    fTimeoutPending = false;
    if (fTimer == nullptr)
        return;
    if (FifoEnabled() && fRxCount > 0)
        fTimer->Arm(host_monotonic_us() + 4 * CharTimeUs());
    else
        fTimer->Disarm();
}


void SerialState::TimerExpired()
{
    if (FifoEnabled() && fRxCount > 0) {
        fTimeoutPending = true;
        UpdateIRQ();
    }
}


/* A character the receiver assembled. With the FIFO full it is lost; with
   the FIFOs off it replaces the one not yet read. Either way that is an
   overrun. */
void SerialState::RxPush(uint8_t ch, uint8_t flags)
{
    if (fRxCount == RxCapacity()) {
        fLsrErrors |= UART_LSR_OE;
        if (FifoEnabled()) {
            UpdateIRQ();
            return;
        }
        fRxCount = 0;
    }
    int tail = (fRxHead + fRxCount) % UART_FIFO_LENGTH;
    fRxData[tail] = ch;
    fRxFlags[tail] = flags;
    fRxCount++;
    if (fRxCount == 1)
        fLsrErrors |= flags;
    RestartTimeout();
    UpdateIRQ();
}


uint8_t SerialState::RxPop()
{
    if (fRxCount == 0)
        return fRbr;
    fRbr = fRxData[fRxHead];
    fRxHead = (fRxHead + 1) % UART_FIFO_LENGTH;
    fRxCount--;
    if (fRxCount > 0)
        fLsrErrors |= fRxFlags[fRxHead];
    RestartTimeout();
    UpdateIRQ();
    return fRbr;
}


void SerialState::RxClear()
{
    fRxHead = 0;
    fRxCount = 0;
    RestartTimeout();
}


/* DCD, RI, DSR and CTS: the modem control outputs in loopback mode, and
   otherwise the inputs, to which nothing is connected. */
uint8_t SerialState::ModemLines() const
{
    if (!(fMcr & UART_MCR_LOOP))
        return 0;
    uint8_t lines = 0;
    if (fMcr & UART_MCR_DTR)
        lines |= UART_MSR_DSR;
    if (fMcr & UART_MCR_RTS)
        lines |= UART_MSR_CTS;
    if (fMcr & UART_MCR_OUT1)
        lines |= UART_MSR_RI;
    if (fMcr & UART_MCR_OUT2)
        lines |= UART_MSR_DCD;
    return lines;
}


void SerialState::SetModemLines(uint8_t lines)
{
    uint8_t changed = (fMsr ^ lines) & 0xf0;
    uint8_t deltas = 0;
    if (changed & UART_MSR_CTS)
        deltas |= UART_MSR_DCTS;
    if (changed & UART_MSR_DSR)
        deltas |= UART_MSR_DDSR;
    if (changed & UART_MSR_DCD)
        deltas |= UART_MSR_DDCD;
    /* only the end of a ring counts */
    if ((changed & UART_MSR_RI) && !(lines & UART_MSR_RI))
        deltas |= UART_MSR_TERI;
    fMsr = lines | (fMsr & UART_MSR_DELTAS) | deltas;
    UpdateIRQ();
}


/* While the FIFOs are off a character overwrites the one not read, so the
   host only offers one when the holding register is empty. In loopback
   mode the receiver is disconnected from the line. */
int SerialState::ReceiveRoom()
{
    if (fMcr & UART_MCR_LOOP)
        return 0;
    return RxCapacity() - fRxCount;
}


void SerialState::Receive(const uint8_t *buf, int len)
{
    len = min_int(len, ReceiveRoom());
    for (int i = 0; i < len; i++)
        RxPush(buf[i], 0);
}


void SerialState::Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;

    switch (offset & 7) {
        case 0:
            if (fLcr & UART_LCR_DLAB) {
                fDivider = (fDivider & 0xff00) | val;
            } else {
                uint8_t ch = val;
                fThrIpending = false;
                UpdateIRQ();

                /* out at once, so the transmitter never stays busy */
                if (fMcr & UART_MCR_LOOP)
                    RxPush(ch, 0);
                else if (fOutput != nullptr)
                    fOutput->WriteData(&ch, 1);
                fThrIpending = true;
                UpdateIRQ();
            }
            break;
        case 1:
            if (fLcr & UART_LCR_DLAB) {
                fDivider = (fDivider & 0x00ff) | (val << 8);
            } else {
                val &= 0x0f;
                if (val & UART_IER_THRI) {
                    if (!(fIer & UART_IER_THRI))
                        fThrIpending = true;
                } else {
                    fThrIpending = false;
                }
                fIer = val;
                UpdateIRQ();
            }
            break;
        case 2:
            /* The other bits are only taken with FIFO enable set, and
               turning the FIFOs on or off empties them. */
            if (!(val & UART_FCR_FE)) {
                if (FifoEnabled())
                    RxClear();
                fFcr = 0;
            } else {
                if (!FifoEnabled() || (val & UART_FCR_RFR))
                    RxClear();
                fFcr = val & (UART_FCR_TRIGGER | UART_FCR_DMA | UART_FCR_FE);
            }
            UpdateIRQ();
            break;
        case 3:
            /* a break starting in loopback mode reaches the receiver as a
               break character */
            if ((val & UART_LCR_SBC) && !(fLcr & UART_LCR_SBC) &&
                (fMcr & UART_MCR_LOOP))
                RxPush(0, UART_LSR_BI);
            fLcr = val;
            break;
        case 4:
            fMcr = val & UART_MCR_MASK;
            SetModemLines(ModemLines());
            break;
        case 5:
        case 6:
            /* read only */
            break;
        case 7:
            fScr = val;
            break;
    }
}


uint32_t SerialState::Read(uint32_t offset, int size_log2)
{
    (void)size_log2;

    int ret = 0;
    switch (offset & 7) {
        case 0:
            if (fLcr & UART_LCR_DLAB) {
                ret = fDivider & 0xff;
            } else {
                ret = RxPop();
                if (fOutput != nullptr)
                    fOutput->TargetReady();
            }
            break;
        case 1:
            if (fLcr & UART_LCR_DLAB) {
                ret = get_bits(fDivider, 8, 8);
            } else {
                ret = fIer;
            }
            break;
        case 2:
            ret = fIir;
            if (FifoEnabled()) {
                ret |= UART_IIR_FE;
            }
            if (fIir == UART_IIR_THRI) {
                fThrIpending = false;
                UpdateIRQ();
            }
            break;
        case 3:
            ret = fLcr;
            break;
        case 4:
            ret = fMcr;
            break;
        case 5:
            ret = UART_LSR_TEMT | UART_LSR_THRE | fLsrErrors;
            if (fRxCount > 0)
                ret |= UART_LSR_DR;
            if (FifoEnabled() && RxErrorsInFifo())
                ret |= UART_LSR_FIFOE;
            fLsrErrors = 0;
            UpdateIRQ();
            break;
        case 6:
            ret = fMsr;
            fMsr &= ~UART_MSR_DELTAS;
            UpdateIRQ();
            break;
        case 7:
            ret = fScr;
            break;
    }
    return ret;
}


//#pragma mark - UartDevice

class UartDevice final: public Device {
private:
    DeviceContext *fCtx;
    /* The port and line the configuration asked for, or -1 for whatever the
       machine hands out. */
    int fPort;
    int fIrqLine;
    SerialState *fSerial = nullptr;
    Resource *fRegs = nullptr;
    Resource *fIrq = nullptr;

public:
    UartDevice(DeviceContext *ctx, int port, int irq):
        Device("serial"), fCtx(ctx), fPort(port), fIrqLine(irq) {}

    ~UartDevice() override {delete fSerial;}

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr) {
            vm_error("%s: must be attached to a system bus\n", Name());
            return false;
        }

        if (sys->RegisterSpace() == RES_IO) {
            /* A port based machine puts its serial ports where its firmware
               and its guests have always looked for them. */
            fRegs = AddFixedResource(RES_IO,
                                     fPort >= 0 ? fPort : UART_PC_COM1_PORT,
                                     UART_PORT_SIZE);
            fIrq = AddFixedResource(RES_IRQ,
                                    fIrqLine >= 0 ? fIrqLine
                                                  : UART_PC_COM1_IRQ, 1);
        } else {
            if (fPort >= 0 || fIrqLine >= 0) {
                vm_error("%s: 'reg' and 'irq' are for a machine that "
                         "addresses its devices by port number; here both "
                         "are allocated\n", Name());
                return false;
            }
            fRegs = AddResource(RES_MMIO, UART_REG_SIZE, 0x1000);
            fIrq = AddResource(RES_IRQ, 1);
        }
        return fRegs != nullptr && fIrq != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        IRQSignal *irq = sys->IrqSignalFor(fIrq->base);

        if (irq == nullptr) {
            vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrq->base);
            return false;
        }
        fSerial = new SerialState(sys->RegisterMap(), fRegs->base, irq,
                                  fCtx->platform->Console(),
                                  fRegs->type == RES_IO ? UART_PC_CLOCK_HZ
                                                        : UART_FDT_CLOCK_HZ,
                                  fCtx->platform->OpenTimer());
        fCtx->serial_input = fSerial;
        return true;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        if (fRegs->type != RES_MMIO) {
            return;
        }
        ctx.fdt->BeginNodeNum("serial", fRegs->base);
        ctx.fdt->PropStr("compatible", "ns16550a");
        ctx.fdt->PropU64Range("reg", fRegs->base, fRegs->size);
        ctx.fdt->PropU32("clock-frequency", UART_FDT_CLOCK_HZ);
        fdt_prop_irq(ctx, fIrq->base);
        ctx.fdt->EndNode();

        /* Claim /chosen's stdout-path from the address that was actually
           assigned, so the path can never name a node that is not there. */
        snprintf(ctx.stdout_path, sizeof(ctx.stdout_path),
                 "/soc/serial@%" PRIx64, fRegs->base);
    }

    void BuildAML(AcpiContext &ctx) override
    {
        if (fRegs->type != RES_IO) {
            return;
        }
        AmlBytes r;
        aml_res_io(r, fRegs->base, fRegs->size);
        aml_res_irq(r, fIrq->base);

        /* the port tells the ports apart */
        AmlBytes dev = aml_name("_HID", aml_eisaid("PNP0501"));
        aml_append(dev, aml_name("_UID", aml_int(fRegs->base)));
        aml_append(dev, aml_name("_CRS", aml_resource_template(r)));
        aml_append(*ctx.aml, aml_device(ctx.NewName("COM").c_str(), dev));
    }
};


//#pragma mark - class

/* On a machine whose devices are addressed by port number the registers go at
   "reg" on line "irq"; on one that maps them into memory both are
   allocated. */
class UartClass final: public DeviceClass {
public:
    UartClass(): DeviceClass("ns16550a") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int port, irq;
        if (!cfg.GetInt("reg", &port, -1) || !cfg.GetInt("irq", &irq, -1)) {
            return nullptr;
        }
        return new UartDevice(ctx, port, irq);
    }
};

static const UartClass sUartClass;
