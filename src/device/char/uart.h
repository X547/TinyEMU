#pragma once

#include <stddef.h>

#include "cutils.h"
#include "device.h"
#include "host_console.h"
#include "iomem.h"

struct DeviceContext;

/* The window the device tree node publishes. Only the first eight bytes
   decode, but a 16550 is conventionally given a page of its own. */
#define UART_REG_SIZE 0x100

/* The eight registers themselves, which is the whole of what a port based
   machine decodes. */
#define UART_PORT_SIZE 8

/* Where COM1 has been since the PC/AT. The other three are at 0x2f8, 0x3e8
   and 0x2e8, on lines 3, 4 and 3; a configuration naming one of those says so
   itself. */
#define UART_PC_COM1_PORT 0x3f8
#define UART_PC_COM1_IRQ  4

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

class SerialState final: public ConsoleTarget {
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
    HostConsole *fOutput = nullptr;

    void UpdateIRQ();

public:
    /* 'output' may be null, which discards what the guest transmits. */
    SerialState(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                HostConsole *output);

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

    /* Host side receive path. There is a single holding register, so the
       host must wait for the guest to take the previous byte. */
    bool CanReceive() const {return (fLsr & UART_LSR_DR) == 0;}
    void ReceiveByte(uint8_t ch);

    void SendBreak();

    /* ConsoleTarget */
    int ReceiveRoom() override {return CanReceive() ? 1 : 0;}
    void Receive(const uint8_t *buf, int len) override;

    DeviceIOAdapter<SerialState, &SerialState::Read, &SerialState::Write> fIo {*this};
};


/* The "ns16550a" configuration node. On a machine whose devices are addressed
   by port number the registers go at 'port' on line 'irq'; on one that maps
   them into memory both are allocated and -1 is what to pass. */
Device *uart_node_create(DeviceContext *ctx, int port, int irq);
