/*
 * QEMU PC keyboard emulation
 *
 * Copyright (c) 2003 Fabrice Bellard
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "iomem.h"
#include "ps2.h"
#include "virtio.h"
#include "machine.h"

/* debug PC keyboard */
//#define DEBUG_KBD

/* debug PC keyboard : only mouse */
//#define DEBUG_MOUSE

/*	Keyboard Controller Commands */
#define KBD_CCMD_READ_MODE	0x20	/* Read mode bits */
#define KBD_CCMD_WRITE_MODE	0x60	/* Write mode bits */
#define KBD_CCMD_GET_VERSION	0xA1	/* Get controller version */
#define KBD_CCMD_MOUSE_DISABLE	0xA7	/* Disable mouse interface */
#define KBD_CCMD_MOUSE_ENABLE	0xA8	/* Enable mouse interface */
#define KBD_CCMD_TEST_MOUSE	0xA9	/* Mouse interface test */
#define KBD_CCMD_SELF_TEST	0xAA	/* Controller self test */
#define KBD_CCMD_KBD_TEST	0xAB	/* Keyboard interface test */
#define KBD_CCMD_KBD_DISABLE	0xAD	/* Keyboard interface disable */
#define KBD_CCMD_KBD_ENABLE	0xAE	/* Keyboard interface enable */
#define KBD_CCMD_READ_INPORT    0xC0    /* read input port */
#define KBD_CCMD_READ_OUTPORT	0xD0    /* read output port */
#define KBD_CCMD_WRITE_OUTPORT	0xD1    /* write output port */
#define KBD_CCMD_WRITE_OBUF	0xD2
#define KBD_CCMD_WRITE_AUX_OBUF	0xD3    /* Write to output buffer as if
					   initiated by the auxiliary device */
#define KBD_CCMD_WRITE_MOUSE	0xD4	/* Write the following byte to the mouse */
#define KBD_CCMD_DISABLE_A20    0xDD    /* HP vectra only ? */
#define KBD_CCMD_ENABLE_A20     0xDF    /* HP vectra only ? */
#define KBD_CCMD_RESET	        0xFE

/* Status Register Bits */
#define KBD_STAT_OBF 		0x01	/* Keyboard output buffer full */
#define KBD_STAT_IBF 		0x02	/* Keyboard input buffer full */
#define KBD_STAT_SELFTEST	0x04	/* Self test successful */
#define KBD_STAT_CMD		0x08	/* Last write was a command write (0=data) */
#define KBD_STAT_UNLOCKED	0x10	/* Zero if keyboard locked */
#define KBD_STAT_MOUSE_OBF	0x20	/* Mouse output buffer full */
#define KBD_STAT_GTO 		0x40	/* General receive/xmit timeout */
#define KBD_STAT_PERR 		0x80	/* Parity error */

/* Controller Mode Register Bits */
#define KBD_MODE_KBD_INT	0x01	/* Keyboard data generate IRQ1 */
#define KBD_MODE_MOUSE_INT	0x02	/* Mouse data generate IRQ12 */
#define KBD_MODE_SYS 		0x04	/* The system flag (?) */
#define KBD_MODE_NO_KEYLOCK	0x08	/* The keylock doesn't affect the keyboard if set */
#define KBD_MODE_DISABLE_KBD	0x10	/* Disable keyboard interface */
#define KBD_MODE_DISABLE_MOUSE	0x20	/* Disable mouse interface */
#define KBD_MODE_KCC 		0x40	/* Scan code conversion to PC format */
#define KBD_MODE_RFU		0x80

#define KBD_PENDING_KBD         1
#define KBD_PENDING_AUX         2

struct KBDState: public PS2IRQTarget {
    uint8_t write_cmd = 0; /* if non zero, write data to port 60 is expected */
    uint8_t status = 0;
    uint8_t mode = 0;
    /* Bitmask of devices with data available.  */
    uint8_t pending = 0;
    PS2KbdState *kbd = nullptr;
    PS2MouseState *mouse = nullptr;

    IRQSignal *irq_kbd = nullptr;
    IRQSignal *irq_mouse = nullptr;

    uint32_t DataRead(uint32_t offset, int size_log2);
    void DataWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t StatusRead(uint32_t offset, int size_log2);
    void CommandWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<KBDState, &KBDState::DataRead,
                    &KBDState::DataWrite> fDataIo {*this};
    DeviceIOAdapter<KBDState, &KBDState::StatusRead,
                    &KBDState::CommandWrite> fCmdIo {*this};

    /* PS2IRQTarget */
    void UpdateKbdIRQ(int level) override;
    void UpdateAuxIRQ(int level) override;
};

static void qemu_system_reset_request(void)
{
    printf("system_reset_request\n");
    exit(1);
    /* XXX */
}

static void ioport_set_a20(int val)
{
}

static int ioport_get_a20(void)
{
    return 1;
}

/* update irq and KBD_STAT_[MOUSE_]OBF */
/* XXX: not generating the irqs if KBD_MODE_DISABLE_KBD is set may be
   incorrect, but it avoids having to simulate exact delays */
static void kbd_update_irq(KBDState *s)
{
    int irq_kbd_level, irq_mouse_level;

    irq_kbd_level = 0;
    irq_mouse_level = 0;
    s->status &= ~(KBD_STAT_OBF | KBD_STAT_MOUSE_OBF);
    if (s->pending) {
        s->status |= KBD_STAT_OBF;
        /* kbd data takes priority over aux data.  */
        if (s->pending == KBD_PENDING_AUX) {
            s->status |= KBD_STAT_MOUSE_OBF;
            if (s->mode & KBD_MODE_MOUSE_INT)
                irq_mouse_level = 1;
        } else {
            if ((s->mode & KBD_MODE_KBD_INT) &&
                !(s->mode & KBD_MODE_DISABLE_KBD))
                irq_kbd_level = 1;
        }
    }
    s->irq_kbd->Set(irq_kbd_level);
    s->irq_mouse->Set(irq_mouse_level);
}

void KBDState::UpdateKbdIRQ(int level)
{
    KBDState *s = this;

    if (level)
        s->pending |= KBD_PENDING_KBD;
    else
        s->pending &= ~KBD_PENDING_KBD;
    kbd_update_irq(s);
}

void KBDState::UpdateAuxIRQ(int level)
{
    KBDState *s = this;

    if (level)
        s->pending |= KBD_PENDING_AUX;
    else
        s->pending &= ~KBD_PENDING_AUX;
    kbd_update_irq(s);
}

uint32_t KBDState::StatusRead(uint32_t addr, int size_log2)
{
    KBDState *s = this;
    int val;
    val = s->status;
#if defined(DEBUG_KBD)
    printf("kbd: read status=0x%02x\n", val);
#endif
    return val;
}

static void kbd_queue(KBDState *s, int b, int aux)
{
    if (aux)
        ps2_queue(s->mouse, b);
    else
        ps2_queue(s->kbd, b);
}

void KBDState::CommandWrite(uint32_t addr, uint32_t val, int size_log2)
{
    KBDState *s = this;

#if defined(DEBUG_KBD)
    printf("kbd: write cmd=0x%02x\n", val);
#endif
    switch(val) {
    case KBD_CCMD_READ_MODE:
        kbd_queue(s, s->mode, 1);
        break;
    case KBD_CCMD_WRITE_MODE:
    case KBD_CCMD_WRITE_OBUF:
    case KBD_CCMD_WRITE_AUX_OBUF:
    case KBD_CCMD_WRITE_MOUSE:
    case KBD_CCMD_WRITE_OUTPORT:
        s->write_cmd = val;
        break;
    case KBD_CCMD_MOUSE_DISABLE:
        s->mode |= KBD_MODE_DISABLE_MOUSE;
        break;
    case KBD_CCMD_MOUSE_ENABLE:
        s->mode &= ~KBD_MODE_DISABLE_MOUSE;
        break;
    case KBD_CCMD_TEST_MOUSE:
        kbd_queue(s, 0x00, 0);
        break;
    case KBD_CCMD_SELF_TEST:
        s->status |= KBD_STAT_SELFTEST;
        kbd_queue(s, 0x55, 0);
        break;
    case KBD_CCMD_KBD_TEST:
        kbd_queue(s, 0x00, 0);
        break;
    case KBD_CCMD_KBD_DISABLE:
        s->mode |= KBD_MODE_DISABLE_KBD;
        kbd_update_irq(s);
        break;
    case KBD_CCMD_KBD_ENABLE:
        s->mode &= ~KBD_MODE_DISABLE_KBD;
        kbd_update_irq(s);
        break;
    case KBD_CCMD_READ_INPORT:
        kbd_queue(s, 0x00, 0);
        break;
    case KBD_CCMD_READ_OUTPORT:
        /* XXX: check that */
        val = 0x01 | (ioport_get_a20() << 1);
        if (s->status & KBD_STAT_OBF)
            val |= 0x10;
        if (s->status & KBD_STAT_MOUSE_OBF)
            val |= 0x20;
        kbd_queue(s, val, 0);
        break;
    case KBD_CCMD_ENABLE_A20:
        ioport_set_a20(1);
        break;
    case KBD_CCMD_DISABLE_A20:
        ioport_set_a20(0);
        break;
    case KBD_CCMD_RESET:
        qemu_system_reset_request();
        break;
    case 0xff:
        /* ignore that - I don't know what is its use */
        break;
    default:
        fprintf(stderr, "qemu: unsupported keyboard cmd=0x%02x\n", val);
        break;
    }
}

uint32_t KBDState::DataRead(uint32_t addr, int size_log2)
{
    KBDState *s = this;
    uint32_t val;
    if (s->pending == KBD_PENDING_AUX)
        val = ps2_read_data(s->mouse);
    else
        val = ps2_read_data(s->kbd);
#ifdef DEBUG_KBD
    printf("kbd: read data=0x%02x\n", val);
#endif
    return val;
}

void KBDState::DataWrite(uint32_t addr, uint32_t val, int size_log2)
{
    KBDState *s = this;

#ifdef DEBUG_KBD
    printf("kbd: write data=0x%02x\n", val);
#endif

    switch(s->write_cmd) {
    case 0:
        ps2_write_keyboard(s->kbd, val);
        break;
    case KBD_CCMD_WRITE_MODE:
        s->mode = val;
        ps2_keyboard_set_translation(s->kbd, (s->mode & KBD_MODE_KCC) != 0);
        /* ??? */
        kbd_update_irq(s);
        break;
    case KBD_CCMD_WRITE_OBUF:
        kbd_queue(s, val, 0);
        break;
    case KBD_CCMD_WRITE_AUX_OBUF:
        kbd_queue(s, val, 1);
        break;
    case KBD_CCMD_WRITE_OUTPORT:
        ioport_set_a20((val >> 1) & 1);
        if (!(val & 1)) {
            qemu_system_reset_request();
        }
        break;
    case KBD_CCMD_WRITE_MOUSE:
        ps2_write_mouse(s->mouse, val);
        break;
    default:
        break;
    }
    s->write_cmd = 0;
}

static void kbd_reset(void *opaque)
{
    KBDState *s = static_cast<KBDState *>(opaque);

    s->mode = KBD_MODE_KBD_INT | KBD_MODE_MOUSE_INT;
    s->status = KBD_STAT_CMD | KBD_STAT_UNLOCKED;
}

KBDState *i8042_init(PS2KbdState **pkbd,
                     PS2MouseState **pmouse,
                     PhysMemoryMap *port_map,
                     IRQSignal *kbd_irq, IRQSignal *mouse_irq, uint32_t io_base)
{
    KBDState *s;
    
    s = new KBDState();
    
    s->irq_kbd = kbd_irq;
    s->irq_mouse = mouse_irq;

    kbd_reset(s);
    port_map->RegisterDevice(io_base, 1, &s->fDataIo, DEVIO_SIZE8);
    port_map->RegisterDevice(io_base + 4, 1, &s->fCmdIo, DEVIO_SIZE8);

    s->kbd = ps2_kbd_init(s);
    s->mouse = ps2_mouse_init(s);

    *pkbd = s->kbd;
    *pmouse = s->mouse;
    return s;
}
