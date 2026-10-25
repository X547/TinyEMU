#pragma once

#include <stddef.h>

#include <memory>

#include "cutils.h"
#include "device.h"
#include "host_console.h"
#include "host_timer.h"
#include "iomem.h"

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

/* The input clocks: the PC's 1.8432 MHz crystal, and what the device tree
   node gives. */
#define UART_PC_CLOCK_HZ  1843200
#define UART_FDT_CLOCK_HZ 3686400

#define UART_LCR_DLAB	0x80	/* Divisor latch access bit */
#define UART_LCR_SBC	0x40	/* Set break control */
#define UART_LCR_PARITY	0x08	/* Parity enable */
#define UART_LCR_STOP	0x04	/* Stop bits: 0=1 bit, 1=1.5 or 2 bits */
#define UART_LCR_WLEN	0x03	/* Word length: 5 to 8 bits */

#define UART_IER_MSI	0x08	/* Enable Modem status interrupt */
#define UART_IER_RLSI	0x04	/* Enable receiver line status interrupt */
#define UART_IER_THRI	0x02	/* Enable Transmitter holding register int. */
#define UART_IER_RDI	0x01	/* Enable receiver data interrupt */

#define UART_IIR_NO_INT	0x01	/* No interrupts pending */
#define UART_IIR_ID	0x0e	/* Mask for the interrupt ID */

#define UART_IIR_MSI	0x00	/* Modem status interrupt */
#define UART_IIR_THRI	0x02	/* Transmitter holding register empty */
#define UART_IIR_RDI	0x04	/* Receiver data interrupt */
#define UART_IIR_RLSI	0x06	/* Receiver line status interrupt */
#define UART_IIR_CTI	0x0c	/* Character timeout indication */
#define UART_IIR_FE     0xC0    /* Fifo enabled */

#define UART_LSR_FIFOE	0x80	/* Error in the receive FIFO */
#define UART_LSR_TEMT	0x40	/* Transmitter empty */
#define UART_LSR_THRE	0x20	/* Transmit-hold-register empty */
#define UART_LSR_BI	0x10	/* Break interrupt indicator */
#define UART_LSR_FE	0x08	/* Frame error indicator */
#define UART_LSR_PE	0x04	/* Parity error indicator */
#define UART_LSR_OE	0x02	/* Overrun error indicator */
#define UART_LSR_DR	0x01	/* Receiver data ready */
/* what reading the register clears, and what raises the line status
   interrupt */
#define UART_LSR_ERRORS (UART_LSR_BI | UART_LSR_FE | UART_LSR_PE | UART_LSR_OE)

#define UART_FCR_TRIGGER    0xC0    /* Receive trigger level: 1, 4, 8, 14 */
#define UART_FCR_DMA        0x08    /* DMA mode select */
#define UART_FCR_XFR        0x04    /* XMIT Fifo Reset */
#define UART_FCR_RFR        0x02    /* RCVR Fifo Reset */
#define UART_FCR_FE         0x01    /* FIFO Enable */

#define UART_MCR_LOOP	0x10	/* Enable loopback test mode */
#define UART_MCR_OUT2	0x08	/* Out2 complement */
#define UART_MCR_OUT1	0x04	/* Out1 complement */
#define UART_MCR_RTS	0x02	/* RTS complement */
#define UART_MCR_DTR	0x01	/* DTR complement */
#define UART_MCR_MASK	0x1f	/* the bits a 16550 has */

#define UART_MSR_DCD	0x80	/* Data Carrier Detect */
#define UART_MSR_RI	0x40	/* Ring Indicator */
#define UART_MSR_DSR	0x20	/* Data Set Ready */
#define UART_MSR_CTS	0x10	/* Clear to Send */
#define UART_MSR_DDCD	0x08	/* Delta DCD */
#define UART_MSR_TERI	0x04	/* Trailing edge ring indicator */
#define UART_MSR_DDSR	0x02	/* Delta DSR */
#define UART_MSR_DCTS	0x01	/* Delta CTS */
#define UART_MSR_DELTAS	0x0f

#define UART_FIFO_LENGTH    16      /* 16550A Fifo Length */

/* A 16550A as the PC16550D data sheet describes it. Characters go out as
   soon as they are written, so the transmitter is always empty; received
   ones wait in the FIFO, or in the holding register while the FIFOs are
   off. Nothing is connected to the modem inputs. */
class SerialState final: public ConsoleTarget, public TimerTarget {
private:
    uint16_t fDivider = 0;
    uint8_t fIer = 0;
    uint8_t fIir = UART_IIR_NO_INT; /* read only */
    uint8_t fLcr = 0;
    uint8_t fMcr = 0;
    /* OE, PE, FE and BI, until the register is read */
    uint8_t fLsrErrors = 0;
    uint8_t fMsr = 0;
    uint8_t fScr = 0;
    uint8_t fFcr = 0;
    /* The transmitter holding register empty interrupt is an event, not the
       state of THRE: it is raised when the register empties or the interrupt
       is enabled while it is empty, and taken back by writing the register
       or by reading IIR while IIR reports it. */
    bool fThrIpending = false;

    /* The receive FIFO, which holds one character while the FIFOs are off.
       Each character keeps the line status bits it arrived with, which
       reach LSR when it gets to the top. */
    uint8_t fRxData[UART_FIFO_LENGTH] {};
    uint8_t fRxFlags[UART_FIFO_LENGTH] {};
    int fRxHead = 0;
    int fRxCount = 0;
    /* what RBR reads once the FIFO is empty */
    uint8_t fRbr = 0;
    /* Four character times passed with characters waiting and nothing
       received or read. */
    bool fTimeoutPending = false;

    uint32_t fClockHz;
    IRQSignal *fIrq = nullptr;
    HostConsole *fOutput = nullptr;
    std::unique_ptr<HostTimer> fTimer;

    void UpdateIRQ();
    bool FifoEnabled() const {return (fFcr & UART_FCR_FE) != 0;}
    int RxCapacity() const {return FifoEnabled() ? UART_FIFO_LENGTH : 1;}
    int RxTrigger() const;
    void RxPush(uint8_t ch, uint8_t flags);
    uint8_t RxPop();
    void RxClear();
    uint8_t RxErrorsInFifo() const;
    void RestartTimeout();
    uint64_t CharTimeUs() const;
    void SetModemLines(uint8_t lines);
    uint8_t ModemLines() const;

public:
    /* 'output' may be null, which discards what the guest transmits, and
       'timer' too, which leaves out the character timeout. */
    SerialState(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                HostConsole *output, uint32_t clock_hz,
                std::unique_ptr<HostTimer> timer);

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

    /* ConsoleTarget */
    int ReceiveRoom() override;
    void Receive(const uint8_t *buf, int len) override;

    /* TimerTarget */
    void TimerExpired() override;

    DeviceIOAdapter<SerialState, &SerialState::Read, &SerialState::Write> fIo {*this};
};
