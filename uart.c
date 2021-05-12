#include "uart.h"
#include <stdint.h>

static void serial_write(void *opaque, uint32_t offset,
                         uint32_t val, int size_log2);
static uint32_t serial_read(void *opaque, uint32_t offset, int size_log2);

SerialState *serial_init(PhysMemoryMap *port_map, int addr,
                         IRQSignal *irq,
                         void (*write_func)(void *opaque, const uint8_t *buf, int buf_len), void *opaque)
{
    SerialState *s;
    s = mallocz(sizeof(*s));
    
    /* all 8 bit registers */
    s->divider = 0; 
    s->rbr = 0; /* receive register */
    s->ier = 0;
    s->iir = UART_IIR_NO_INT; /* read only */
    s->lcr = 0;
    s->mcr = 0;
    s->lsr = UART_LSR_TEMT | UART_LSR_THRE; /* read only */
    s->msr = 0;
    s->scr = 0;
    s->fcr = 0;

    s->irq = irq;
    s->write_func = write_func;
    s->opaque = opaque;

    cpu_register_device(port_map, addr, 8, s, serial_read, serial_write, 
                        DEVIO_SIZE8);
    return s;
}

static void serial_update_irq(SerialState *s)
{
    if ((s->lsr & UART_LSR_DR) && (s->ier & UART_IER_RDI)) {
        s->iir = UART_IIR_RDI;
    } else if ((s->lsr & UART_LSR_THRE) && (s->ier & UART_IER_THRI)) {
        s->iir = UART_IIR_THRI;
    } else {
        s->iir = UART_IIR_NO_INT;
    }
    if (s->iir != UART_IIR_NO_INT) {
        set_irq(s->irq, 1);
    } else {
        set_irq(s->irq, 0);
    }
}

#if 0
/* send remainining chars in fifo */
Serial.prototype.write_tx_fifo = function()
{
    if (s->tx_fifo != "") {
        s->write_func(s->tx_fifo);
        s->tx_fifo = "";
        
        s->lsr |= UART_LSR_THRE;
        s->lsr |= UART_LSR_TEMT;
        s->update_irq();
    }
}
#endif
    
static void serial_write(void *opaque, uint32_t offset,
                         uint32_t val, int size_log2)
{
    SerialState *s = opaque;
    int addr;

    addr = offset & 7;
    switch(addr) {
    default:
    case 0:
        if (s->lcr & UART_LCR_DLAB) {
            s->divider = (s->divider & 0xff00) | val;
        } else {
#if 0
            if (s->fcr & UART_FCR_FE) {
                s->tx_fifo += String.fromCharCode(val);
                s->lsr &= ~UART_LSR_THRE;
                serial_update_irq(s);
                if (s->tx_fifo.length >= UART_FIFO_LENGTH) {
                    /* write to the terminal */
                    s->write_tx_fifo();
                }
            } else
#endif
            {
                uint8_t ch;
                s->lsr &= ~UART_LSR_THRE;
                serial_update_irq(s);
                
                /* write to the terminal */
                ch = val;
                s->write_func(s->opaque, &ch, 1);
                s->lsr |= UART_LSR_THRE;
                s->lsr |= UART_LSR_TEMT;
                serial_update_irq(s);
            }
        }
        break;
    case 1:
        if (s->lcr & UART_LCR_DLAB) {
            s->divider = (s->divider & 0x00ff) | (val << 8);
        } else {
            s->ier = val;
            serial_update_irq(s);
        }
        break;
    case 2:
#if 0
        if ((s->fcr ^ val) & UART_FCR_FE) {
            /* clear fifos */
            val |= UART_FCR_XFR | UART_FCR_RFR;
        }
        if (val & UART_FCR_XFR)
            s->tx_fifo = "";
        if (val & UART_FCR_RFR)
            s->rx_fifo = "";
        s->fcr = val & UART_FCR_FE;
#endif
        break;
    case 3:
        s->lcr = val;
        break;
    case 4:
        s->mcr = val;
        break;
    case 5:
        break;
    case 6:
        s->msr = val;
        break;
    case 7:
        s->scr = val;
        break;
    }
}

static uint32_t serial_read(void *opaque, uint32_t offset, int size_log2)
{
    SerialState *s = opaque;
    int ret, addr;

    addr = offset & 7;
    switch(addr) {
    default:
    case 0:
        if (s->lcr & UART_LCR_DLAB) {
            ret = s->divider & 0xff; 
        } else {
            ret = s->rbr;
            s->lsr &= ~(UART_LSR_DR | UART_LSR_BI);
            serial_update_irq(s);
#if 0
            /* try to receive next chars */
            s->send_char_from_fifo();
#endif
        }
        break;
    case 1:
        if (s->lcr & UART_LCR_DLAB) {
            ret = (s->divider >> 8) & 0xff;
        } else {
            ret = s->ier;
        }
        break;
    case 2:
        ret = s->iir;
        if (s->fcr & UART_FCR_FE)
            ret |= UART_IIR_FE;
        break;
    case 3:
        ret = s->lcr;
        break;
    case 4:
        ret = s->mcr;
        break;
    case 5:
        ret = s->lsr;
        break;
    case 6:
        ret = s->msr;
        break;
    case 7:
        ret = s->scr;
        break;
    }
    return ret;
}

void serial_send_break(SerialState *s)
{
    s->rbr = 0;
    s->lsr |= UART_LSR_BI | UART_LSR_DR;
    serial_update_irq(s);
}

#if 0
static void serial_send_char(SerialState *s, int ch)
{
    s->rbr = ch;
    s->lsr |= UART_LSR_DR;
    serial_update_irq(s);
}

Serial.prototype.send_char_from_fifo = function()
{
    var fifo;

    fifo = s->rx_fifo;
    if (fifo != "" && !(s->lsr & UART_LSR_DR)) {
        s->send_char(fifo.charCodeAt(0));
        s->rx_fifo = fifo.substr(1, fifo.length - 1);
    }
}

/* queue the string in the UART receive fifo and send it ASAP */
Serial.prototype.send_chars = function(str)
{
    s->rx_fifo += str;
    s->send_char_from_fifo();
}
    
#endif
