/*
 * Intel 8042 PS/2 keyboard controller
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
#include "i8042.h"

#include <stdlib.h>
#include <stdio.h>

#include "devices.h"
#include "machine.h"
#include "vmmouse.h"

/* debug PC keyboard */
//#define DEBUG_KBD

/* Keyboard controller commands */
#define KBD_CCMD_READ_MODE      0x20 /* Read mode bits */
#define KBD_CCMD_WRITE_MODE     0x60 /* Write mode bits */
#define KBD_CCMD_GET_VERSION    0xa1 /* Get controller version */
#define KBD_CCMD_MOUSE_DISABLE  0xa7 /* Disable mouse interface */
#define KBD_CCMD_MOUSE_ENABLE   0xa8 /* Enable mouse interface */
#define KBD_CCMD_TEST_MOUSE     0xa9 /* Mouse interface test */
#define KBD_CCMD_SELF_TEST      0xaa /* Controller self test */
#define KBD_CCMD_KBD_TEST       0xab /* Keyboard interface test */
#define KBD_CCMD_KBD_DISABLE    0xad /* Keyboard interface disable */
#define KBD_CCMD_KBD_ENABLE     0xae /* Keyboard interface enable */
#define KBD_CCMD_READ_INPORT    0xc0 /* read input port */
#define KBD_CCMD_READ_OUTPORT   0xd0 /* read output port */
#define KBD_CCMD_WRITE_OUTPORT  0xd1 /* write output port */
#define KBD_CCMD_WRITE_OBUF     0xd2
#define KBD_CCMD_WRITE_AUX_OBUF 0xd3 /* Write to output buffer as if
                                        initiated by the auxiliary device */
#define KBD_CCMD_WRITE_MOUSE    0xd4 /* Write the next byte to the mouse */
#define KBD_CCMD_DISABLE_A20    0xdd /* HP vectra only ? */
#define KBD_CCMD_ENABLE_A20     0xdf /* HP vectra only ? */
#define KBD_CCMD_RESET          0xfe

/* Status Register Bits */
#define KBD_STAT_OBF       0x01 /* Keyboard output buffer full */
#define KBD_STAT_IBF       0x02 /* Keyboard input buffer full */
#define KBD_STAT_SELFTEST  0x04 /* Self test successful */
#define KBD_STAT_CMD       0x08 /* Last write was a command write (0=data) */
#define KBD_STAT_UNLOCKED  0x10 /* Zero if keyboard locked */
#define KBD_STAT_MOUSE_OBF 0x20 /* Mouse output buffer full */
#define KBD_STAT_GTO       0x40 /* General receive/xmit timeout */
#define KBD_STAT_PERR      0x80 /* Parity error */

/* Controller Mode Register Bits */
#define KBD_MODE_KBD_INT       0x01 /* Keyboard data generate IRQ1 */
#define KBD_MODE_MOUSE_INT     0x02 /* Mouse data generate IRQ12 */
#define KBD_MODE_SYS           0x04 /* The system flag (?) */
#define KBD_MODE_NO_KEYLOCK    0x08 /* The keylock does not affect the kbd */
#define KBD_MODE_DISABLE_KBD   0x10 /* Disable keyboard interface */
#define KBD_MODE_DISABLE_MOUSE 0x20 /* Disable mouse interface */
#define KBD_MODE_KCC           0x40 /* Scan code conversion to PC format */
#define KBD_MODE_RFU           0x80

#define KBD_PENDING_KBD (1 << I8042_PORT_KBD)
#define KBD_PENDING_AUX (1 << I8042_PORT_AUX)


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


//#pragma mark - construction

I8042Controller::I8042Controller(PhysMemoryMap *port_map, IRQSignal *kbd_irq,
                                 IRQSignal *aux_irq, uint32_t io_base):
    fKbdIrq(kbd_irq),
    fAuxIrq(aux_irq)
{
    for (int i = 0; i < I8042_PORT_COUNT; i++) {
        fPorts[i].owner = this;
        fPorts[i].index = i;
        fPorts[i].bus = new PS2Bus(nullptr, &fPorts[i]);
    }

    /* A PC's controller comes up translating, and the keyboard behind it
       comes up in set 2, so a guest that never writes the mode register
       still reads the set 1 codes its software is written against. */
    fMode = KBD_MODE_KBD_INT | KBD_MODE_MOUSE_INT | KBD_MODE_KCC;
    fStatus = KBD_STAT_CMD | KBD_STAT_UNLOCKED;

    port_map->RegisterDevice(io_base, 1, &fDataIo, DEVIO_SIZE8);
    port_map->RegisterDevice(io_base + 4, 1, &fCmdIo, DEVIO_SIZE8);
}


I8042Controller::~I8042Controller()
{
    /* The bus owns whatever was attached to it. */
    for (int i = 0; i < I8042_PORT_COUNT; i++) {
        delete fPorts[i].bus;
    }
}


PS2Bus *I8042Controller::PortBus(int port)
{
    if (port < 0 || port >= I8042_PORT_COUNT) {
        return nullptr;
    }
    return fPorts[port].bus;
}


bool I8042Controller::Port::AttachDevice(PS2Device *new_dev)
{
    if (dev != nullptr) {
        vm_error("i8042: port %d already carries a device\n", index);
        return false;
    }
    dev = new_dev;
    new_dev->SetPort(this);
    return true;
}


//#pragma mark - interrupts

void I8042Controller::Port::PS2DataAvailable(bool available)
{
    if (available) {
        owner->fPending |= 1 << index;
    } else {
        owner->fPending &= ~(1 << index);
    }
    owner->UpdateIRQ();
}


/* update irq and KBD_STAT_[MOUSE_]OBF */
/* XXX: not generating the irqs if KBD_MODE_DISABLE_KBD is set may be
   incorrect, but it avoids having to simulate exact delays */
void I8042Controller::UpdateIRQ()
{
    int irq_kbd_level = 0;
    int irq_mouse_level = 0;

    fStatus &= ~(KBD_STAT_OBF | KBD_STAT_MOUSE_OBF);
    if (fPending) {
        fStatus |= KBD_STAT_OBF;
        /* kbd data takes priority over aux data. */
        if (fPending == KBD_PENDING_AUX) {
            fStatus |= KBD_STAT_MOUSE_OBF;
            if (fMode & KBD_MODE_MOUSE_INT)
                irq_mouse_level = 1;
        } else {
            if ((fMode & KBD_MODE_KBD_INT) &&
                !(fMode & KBD_MODE_DISABLE_KBD))
                irq_kbd_level = 1;
        }
    }
    fKbdIrq->Set(irq_kbd_level);
    fAuxIrq->Set(irq_mouse_level);
}


//#pragma mark - scancode translation

/* Convert one set 2 code into the set 1 the PC's software expects. The
   correspondence itself lives with the bus, so the keyboard's set 1 to set 2
   direction and this one are two views of the same table and the round trip
   is exact. A code with no counterpart is passed through rather than
   dropped, because a guest can tell something arrived either way. */
bool I8042Controller::Translate(uint8_t val, uint8_t *out)
{
    if (val == PS2_SCAN_EXTEND) {
        /* The prefix means the same in both sets and comes before the
           release prefix, so it is handed over as it stands. */
        *out = val;
        return true;
    }
    if (val == PS2_SCAN_RELEASE) {
        /* Set 1 has no release prefix: the top bit of the code that follows
           carries it instead. */
        fXlateBreak = true;
        return false;
    }
    uint8_t set1 = ps2_set2_to_set1(val);
    if (set1 == 0) {
        fXlateBreak = false;
        *out = val;
        return true;
    }
    if (fXlateBreak) {
        set1 |= 0x80;
        fXlateBreak = false;
    }
    *out = set1;
    return true;
}


/* One byte out of a port's device, translated when the mode register asks
   for it. */
uint8_t I8042Controller::ReadFromPort(int port)
{
    PS2Device *dev = fPorts[port].dev;
    if (dev == nullptr) {
        return 0;
    }

    for (;;) {
        bool is_scancode = false;
        uint8_t val = dev->Read(&is_scancode);
        if (!is_scancode || (fMode & KBD_MODE_KCC) == 0) {
            /* Replies -- acknowledgements, identifiers, the scancode set a
               keyboard reports -- are not scancodes and go through as they
               are, and so does everything when translation is off. */
            return val;
        }
        uint8_t out;
        if (Translate(val, &out)) {
            return out;
        }
        /* A swallowed release prefix. The code it belongs to was queued with
           it, so it is there; if it somehow is not, hand the prefix over
           rather than spinning. */
        if (!dev->HasData()) {
            fXlateBreak = false;
            return val;
        }
    }
}


/* A byte the controller itself puts in a port's output, as it does for its
   own self test result. */
void I8042Controller::QueueFromController(uint8_t val, int port)
{
    PS2Device *dev = fPorts[port].dev;
    if (dev != nullptr) {
        dev->Queue(val);
    }
}


//#pragma mark - registers

uint32_t I8042Controller::StatusRead(uint32_t addr, int size_log2)
{
    uint32_t val = fStatus;
#if defined(DEBUG_KBD)
    printf("kbd: read status=0x%02x\n", val);
#endif
    return val;
}


void I8042Controller::CommandWrite(uint32_t addr, uint32_t val, int size_log2)
{
#if defined(DEBUG_KBD)
    printf("kbd: write cmd=0x%02x\n", val);
#endif
    switch (val) {
    case KBD_CCMD_READ_MODE:
        QueueFromController(fMode, I8042_PORT_AUX);
        break;
    case KBD_CCMD_WRITE_MODE:
    case KBD_CCMD_WRITE_OBUF:
    case KBD_CCMD_WRITE_AUX_OBUF:
    case KBD_CCMD_WRITE_MOUSE:
    case KBD_CCMD_WRITE_OUTPORT:
        fWriteCmd = val;
        break;
    case KBD_CCMD_MOUSE_DISABLE:
        fMode |= KBD_MODE_DISABLE_MOUSE;
        break;
    case KBD_CCMD_MOUSE_ENABLE:
        fMode &= ~KBD_MODE_DISABLE_MOUSE;
        break;
    case KBD_CCMD_TEST_MOUSE:
        QueueFromController(0x00, I8042_PORT_KBD);
        break;
    case KBD_CCMD_SELF_TEST:
        fStatus |= KBD_STAT_SELFTEST;
        QueueFromController(0x55, I8042_PORT_KBD);
        break;
    case KBD_CCMD_KBD_TEST:
        QueueFromController(0x00, I8042_PORT_KBD);
        break;
    case KBD_CCMD_KBD_DISABLE:
        fMode |= KBD_MODE_DISABLE_KBD;
        UpdateIRQ();
        break;
    case KBD_CCMD_KBD_ENABLE:
        fMode &= ~KBD_MODE_DISABLE_KBD;
        UpdateIRQ();
        break;
    case KBD_CCMD_READ_INPORT:
        QueueFromController(0x00, I8042_PORT_KBD);
        break;
    case KBD_CCMD_READ_OUTPORT:
        /* XXX: check that */
        val = 0x01 | (ioport_get_a20() << 1);
        if (fStatus & KBD_STAT_OBF)
            val |= 0x10;
        if (fStatus & KBD_STAT_MOUSE_OBF)
            val |= 0x20;
        QueueFromController(val, I8042_PORT_KBD);
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


uint32_t I8042Controller::DataRead(uint32_t addr, int size_log2)
{
    uint32_t val;
    if (fPending == KBD_PENDING_AUX)
        val = ReadFromPort(I8042_PORT_AUX);
    else
        val = ReadFromPort(I8042_PORT_KBD);
#ifdef DEBUG_KBD
    printf("kbd: read data=0x%02x\n", val);
#endif
    return val;
}


void I8042Controller::DataWrite(uint32_t addr, uint32_t val, int size_log2)
{
#ifdef DEBUG_KBD
    printf("kbd: write data=0x%02x\n", val);
#endif
    PS2Device *kbd = fPorts[I8042_PORT_KBD].dev;
    PS2Device *aux = fPorts[I8042_PORT_AUX].dev;

    switch (fWriteCmd) {
    case 0:
        if (kbd != nullptr)
            kbd->Write(val);
        break;
    case KBD_CCMD_WRITE_MODE:
        fMode = val;
        /* Whether the translator is in circuit has just changed, so a
           release prefix half way through conversion no longer applies. */
        fXlateBreak = false;
        /* ??? */
        UpdateIRQ();
        break;
    case KBD_CCMD_WRITE_OBUF:
        QueueFromController(val, I8042_PORT_KBD);
        break;
    case KBD_CCMD_WRITE_AUX_OBUF:
        QueueFromController(val, I8042_PORT_AUX);
        break;
    case KBD_CCMD_WRITE_OUTPORT:
        ioport_set_a20((val >> 1) & 1);
        if (!(val & 1)) {
            qemu_system_reset_request();
        }
        break;
    case KBD_CCMD_WRITE_MOUSE:
        if (aux != nullptr)
            aux->Write(val);
        break;
    default:
        break;
    }
    fWriteCmd = 0;
}


//#pragma mark - factory

I8042Controller *i8042_init(PS2Keyboard **pkbd, PS2Mouse **pmouse,
                            PhysMemoryMap *port_map, IRQSignal *kbd_irq,
                            IRQSignal *aux_irq, uint32_t io_base)
{
    I8042Controller *s = new I8042Controller(port_map, kbd_irq, aux_irq,
                                             io_base);

    /* The PC's topology is fixed, so the two devices are put on the two
       ports here rather than being declared. They go on through the bus all
       the same, which is the path a configuration driven controller uses. */
    PS2Keyboard *kbd = ps2_keyboard_create();
    PS2Mouse *mouse = ps2_mouse_create();

    if (!s->PortBus(I8042_PORT_KBD)->AddDevice(
            std::make_unique<PS2DeviceNode>("ps2-keyboard", kbd)) ||
        !s->PortBus(I8042_PORT_KBD)->RealizeAll() ||
        !s->PortBus(I8042_PORT_AUX)->AddDevice(
            std::make_unique<PS2DeviceNode>("ps2-mouse", mouse)) ||
        !s->PortBus(I8042_PORT_AUX)->RealizeAll()) {
        delete s;
        return nullptr;
    }

    *pkbd = kbd;
    *pmouse = mouse;
    return s;
}


//#pragma mark - the configuration node

/* Routes the window's input events to whichever of the two protocols the
   guest is driving: key events always reach the keyboard, and pointer events
   go through the backdoor, which passes them on to the PS/2 pointer for as
   long as no driver has turned the absolute protocol on. */
class I8042Input final: public InputEventTarget, public VMPortTarget {
public:
    PS2Keyboard *kbd = nullptr;
    PS2Mouse *mouse = nullptr;
    VMMousePtr vmmouse;

    void SendKeyEvent(bool is_down, uint16_t key_code) override
    {
        kbd->PutKeycode(is_down, key_code);
    }

    void SendMouseEvent(int dx, int dy, int dz,
                        unsigned int buttons) override
    {
        if (vmmouse != nullptr) {
            vmmouse_send_mouse_event(vmmouse.get(), dx, dy, dz, buttons);
        } else {
            mouse->MouseEvent(dx, dy, dz, buttons);
        }
    }

    bool MouseIsAbsolute() override
    {
        return vmmouse != nullptr && vmmouse_is_absolute(vmmouse.get());
    }

    void VMPortCommand(uint32_t *regs) override
    {
        vmmouse_handler(vmmouse.get(), regs);
    }
};


class I8042Device final: public Device {
private:
    DeviceContext *fCtx;
    bool fVmmouse;
    std::unique_ptr<I8042Controller> fController;
    I8042Input fInput;
    Resource *fDataRes = nullptr;
    Resource *fCmdRes = nullptr;
    Resource *fVmportRes = nullptr;
    Resource *fKbdIrqRes = nullptr;
    Resource *fAuxIrqRes = nullptr;

public:
    I8042Device(DeviceContext *ctx, bool vmmouse):
        Device("i8042"), fCtx(ctx), fVmmouse(vmmouse) {}

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr || !sys->IsPortBased()) {
            vm_error("%s: must be attached to a PC system bus\n", Name());
            return false;
        }

        fDataRes = AddFixedResource(RES_IO, I8042_IO_BASE, 1);
        fCmdRes = AddFixedResource(RES_IO, I8042_IO_BASE + 4, 1);
        fKbdIrqRes = AddFixedResource(RES_IRQ, I8042_IRQ_KBD, 1);
        fAuxIrqRes = AddFixedResource(RES_IRQ, I8042_IRQ_AUX, 1);
        if (fDataRes == nullptr || fCmdRes == nullptr ||
            fKbdIrqRes == nullptr || fAuxIrqRes == nullptr) {
            return false;
        }
        if (fVmmouse) {
            fVmportRes = AddFixedResource(RES_IO, I8042_VMPORT, 1);
            if (fVmportRes == nullptr) {
                return false;
            }
        }
        return true;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        PS2Keyboard *kbd;
        PS2Mouse *mouse;

        fController.reset(i8042_init(&kbd, &mouse, sys->PortMap(),
                                     sys->IrqSignalFor(fKbdIrqRes->base),
                                     sys->IrqSignalFor(fAuxIrqRes->base),
                                     fDataRes->base));
        if (fController == nullptr) {
            return false;
        }

        fInput.kbd = kbd;
        fInput.mouse = mouse;
        if (fVmmouse) {
            fInput.vmmouse = vmmouse_init(mouse);
            fCtx->vmport = &fInput;
            fCtx->vmport_base = fVmportRes->base;
        }
        fCtx->keyboard = &fInput;
        fCtx->mouse = &fInput;
        return true;
    }
};


Device *i8042_node_create(DeviceContext *ctx, bool vmmouse)
{
    return new I8042Device(ctx, vmmouse);
}
