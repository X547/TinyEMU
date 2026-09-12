#pragma once

#include <stddef.h>

#include "cutils.h"
#include "device.h"
#include "iomem.h"

struct DeviceContext;

/* The window the device tree node publishes. Only the first eight bytes
   decode, but a 16550 is conventionally given a page of its own. */
#define UART_REG_SIZE 0x100

#define UART_LCR_DLAB	0x80	/* Divisor latch access bit */

#define UART_IER_MSI	0x08	/* Enable Modem status interrupt */
#define UART_IER_RLSI	0x04	/* Enable receiver line status interrupt */
#define UART_IER_THRI	0x02	/* Enable Transmitter holding register int. */
#define UART_IER_RDI	0x01	/* Enable receiver data interrupt */

#define UART_IIR_NO_INT	0x01	/* No interrupts pending */
#define UART_IIR_ID	0x06	/* Mask for the interrupt ID */

#define UART_IIR_MSI	0x00	/* Modem status interrupt */
#define UART_IIR_THRI	0x02	/* Transmitter holding register empty */
#define UART_IIR_RDI	0x04	/* Receiver data interrupt */
#define UART_IIR_RLSI	0x06	/* Receiver line status interrupt */
#define UART_IIR_FE     0xC0    /* Fifo enabled */

#define UART_LSR_TEMT	0x40	/* Transmitter empty */
#define UART_LSR_THRE	0x20	/* Transmit-hold-register empty */
#define UART_LSR_BI	0x10	/* Break interrupt indicator */
#define UART_LSR_FE	0x08	/* Frame error indicator */
#define UART_LSR_PE	0x04	/* Parity error indicator */
#define UART_LSR_OE	0x02	/* Overrun error indicator */
#define UART_LSR_DR	0x01	/* Receiver data ready */

#define UART_FCR_XFR        0x04    /* XMIT Fifo Reset */
#define UART_FCR_RFR        0x02    /* RCVR Fifo Reset */
#define UART_FCR_FE         0x01    /* FIFO Enable */

#define UART_FIFO_LENGTH    16      /* 16550A Fifo Length */

/* Implemented by whoever consumes the bytes the guest transmits. */
class SerialOutput {
public:
    virtual ~SerialOutput() = default;

    virtual void WriteData(const uint8_t *buf, int buf_len) = 0;
};


class SerialState {
private:
    uint8_t fDivider = 0;
    uint8_t fRbr = 0; /* receive register */
    uint8_t fIer = 0;
    uint8_t fIir = UART_IIR_NO_INT; /* read only */
    uint8_t fLcr = 0;
    uint8_t fMcr = 0;
    uint8_t fLsr = UART_LSR_TEMT | UART_LSR_THRE; /* read only */
    uint8_t fMsr = 0;
    uint8_t fScr = 0;
    uint8_t fFcr = 0;

    IRQSignal *fIrq = nullptr;
    SerialOutput *fOutput = nullptr;

    void UpdateIRQ();

public:
    SerialState(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                SerialOutput *output);

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

    /* Host side receive path. There is a single holding register, so the
       host must wait for the guest to take the previous byte. */
    bool CanReceive() const {return (fLsr & UART_LSR_DR) == 0;}
    void ReceiveByte(uint8_t ch);

    void SendBreak();

    DeviceIOAdapter<SerialState, &SerialState::Read, &SerialState::Write> fIo {*this};
};


Device *uart_node_create(DeviceContext *ctx, SerialOutput *output);
