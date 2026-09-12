#include "uart.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "devices.h"
#include "fdt.h"


SerialState::SerialState(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                         SerialOutput *output):
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
    SerialOutput *fOutput;
    SerialState *fSerial = nullptr;
    Resource *fMmio = nullptr;
    Resource *fIrq = nullptr;

public:
    UartDevice(DeviceContext *ctx, SerialOutput *output):
        Device("serial"), fCtx(ctx), fOutput(output) {}

    ~UartDevice() override {delete fSerial;}

    bool Prepare() override
    {
        fMmio = AddResource(RES_MMIO, UART_REG_SIZE, 0x1000);
        fIrq = AddResource(RES_IRQ, 1);
        return fMmio != nullptr && fIrq != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        fSerial = new SerialState(sys->MemMap(), fMmio->base,
                                  sys->IrqSignalFor(fIrq->base), fOutput);
        fCtx->serial_console = fSerial;
        return true;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        ctx.fdt->BeginNodeNum("serial", fMmio->base);
        ctx.fdt->PropStr("compatible", "ns16550a");
        ctx.fdt->PropU64Range("reg", fMmio->base, fMmio->size);
        ctx.fdt->PropU32("clock-frequency", 3686400);
        fdt_prop_plic_irq(ctx, fIrq->base);
        ctx.fdt->EndNode();

        /* Claim /chosen's stdout-path from the address that was actually
           assigned, so the path can never name a node that is not there. */
        snprintf(ctx.stdout_path, sizeof(ctx.stdout_path),
                 "/soc/serial@%" PRIx64, fMmio->base);
    }
};


//#pragma mark - factory

Device *uart_node_create(DeviceContext *ctx, SerialOutput *output)
{
    return new UartDevice(ctx, output);
}
