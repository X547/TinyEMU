#include "uart.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "fdt.h"


SerialState::SerialState(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                         HostConsole *output):
    fIrq(irq),
    fOutput(output)
{
    port_map->RegisterDevice(addr, 8, &fIo, DEVIO_SIZE8);
}


void SerialState::UpdateIRQ()
{
    if ((fLsr & UART_LSR_DR) && (fIer & UART_IER_RDI)) {
        fIir = UART_IIR_RDI;
    } else if ((fLsr & UART_LSR_THRE) && (fIer & UART_IER_THRI)) {
        fIir = UART_IIR_THRI;
    } else {
        fIir = UART_IIR_NO_INT;
    }
    if (fIir != UART_IIR_NO_INT) {
        fIrq->Set(1);
    } else {
        fIrq->Set(0);
    }
}


void SerialState::ReceiveByte(uint8_t ch)
{
    fRbr = ch;
    fLsr |= UART_LSR_DR;
    UpdateIRQ();
}


void SerialState::Receive(const uint8_t *buf, int len)
{
    if (len > 0) {
        ReceiveByte(buf[0]);
    }
}


void SerialState::Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;

    switch (offset & 7) {
        case 0:
            if (fLcr & UART_LCR_DLAB) {
                fDivider = (fDivider & 0xff00) | val;
            } else {
                uint8_t ch;
                fLsr &= ~UART_LSR_THRE;
                UpdateIRQ();

                /* write to the terminal */
                ch = val;
                if (fOutput != nullptr)
                    fOutput->WriteData(&ch, 1);
                fLsr |= UART_LSR_THRE;
                fLsr |= UART_LSR_TEMT;
                UpdateIRQ();
            }
            break;
        case 1:
            if (fLcr & UART_LCR_DLAB) {
                fDivider = (fDivider & 0x00ff) | (val << 8);
            } else {
                fIer = val;
                UpdateIRQ();
            }
            break;
        case 2:
            break;
        case 3:
            fLcr = val;
            break;
        case 4:
            fMcr = val;
            break;
        case 5:
            break;
        case 6:
            fMsr = val;
            break;
        case 7:
            fScr = val;
            break;
        default:
            break;
    }
}


uint32_t SerialState::Read(uint32_t offset, int size_log2)
{
    (void)size_log2;

    int ret;
    switch (offset & 7) {
        case 0:
            if (fLcr & UART_LCR_DLAB) {
                ret = fDivider & 0xff;
            } else {
                ret = fRbr;
                fLsr &= ~(UART_LSR_DR | UART_LSR_BI);
                UpdateIRQ();
                if (fOutput != nullptr)
                    fOutput->TargetReady();
            }
            break;
        case 1:
            if (fLcr & UART_LCR_DLAB) {
                ret = (fDivider >> 8) & 0xff;
            } else {
                ret = fIer;
            }
            break;
        case 2:
            ret = fIir;
            if (fFcr & UART_FCR_FE) {
                ret |= UART_IIR_FE;
            }
            break;
        case 3:
            ret = fLcr;
            break;
        case 4:
            ret = fMcr;
            break;
        case 5:
            ret = fLsr;
            break;
        case 6:
            ret = fMsr;
            break;
        case 7:
            ret = fScr;
            break;
        default:
            ret = 0;
            break;
    }
    return ret;
}


void SerialState::SendBreak()
{
    fRbr = 0;
    fLsr |= UART_LSR_BI | UART_LSR_DR;
    UpdateIRQ();
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
                                  fCtx->platform->Console());
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
        ctx.fdt->PropU32("clock-frequency", 3686400);
        fdt_prop_irq(ctx, fIrq->base);
        ctx.fdt->EndNode();

        /* Claim /chosen's stdout-path from the address that was actually
           assigned, so the path can never name a node that is not there. */
        snprintf(ctx.stdout_path, sizeof(ctx.stdout_path),
                 "/soc/serial@%" PRIx64, fRegs->base);
    }
};


//#pragma mark - factory

Device *uart_node_create(DeviceContext *ctx, int port, int irq)
{
    return new UartDevice(ctx, port, irq);
}
