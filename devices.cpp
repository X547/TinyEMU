/*
 * Configurable device objects
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
#include "devices.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "cutils.h"
#include "fdt.h"
#include "nvme.h"
#include "pci_host_dw.h"
#include "pci_host_ecam.h"
#include "scsi.h"
#include "usb.h"
#include "xhci.h"

#define UART_REG_SIZE 0x100
#define FB_ALLOC_ALIGN 65536


/* Emit the "interrupts-extended" property naming the PLIC line a resource was
   assigned. Kept in one place so that every device describes the line it
   actually got. */
static void fdt_prop_plic_irq(FDTContext &ctx, uint64_t line)
{
    uint32_t tab[2];
    tab[0] = ctx.plic_phandle;
    tab[1] = line;
    ctx.fdt->PropTabU32("interrupts-extended", tab, 2);
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


//#pragma mark - SimpleFBDevice

class SimpleFBDevice final: public Device {
private:
    DeviceContext *fCtx;
    int fWidth;
    int fHeight;
    Resource *fMmio = nullptr;
    FBDevice *fFb = nullptr;

public:
    SimpleFBDevice(DeviceContext *ctx, int width, int height):
        Device("framebuffer"), fCtx(ctx), fWidth(width), fHeight(height) {}

    bool Prepare() override
    {
        /* simplefb_init() rounds the allocation the same way; computing it
           here keeps the reservation and the mapping identical. */
        uint64_t size = (uint64_t)fHeight * fWidth * 4;
        size = (size + FB_ALLOC_ALIGN - 1) & ~(uint64_t)(FB_ALLOC_ALIGN - 1);
        fMmio = AddResource(RES_MMIO, size, FB_ALLOC_ALIGN);
        return fMmio != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        fFb = simplefb_init(sys->MemMap(), fMmio->base, fWidth, fHeight);
        fCtx->fb_dev = fFb;
        return fFb != nullptr;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        ctx.fdt->BeginNodeNum("framebuffer", fMmio->base);
        ctx.fdt->PropStr("compatible", "simple-framebuffer");
        ctx.fdt->PropU64Range("reg", fMmio->base, fFb->fb_size);
        ctx.fdt->PropU32("width", fFb->width);
        ctx.fdt->PropU32("height", fFb->height);
        ctx.fdt->PropU32("stride", fFb->stride);
        ctx.fdt->PropStr("format", "a8r8g8b8");
        ctx.fdt->EndNode();
    }
};


//#pragma mark - VirtioDevice

typedef enum {
    VIRTIO_KIND_BLOCK,
    VIRTIO_KIND_NET,
    VIRTIO_KIND_CONSOLE,
    VIRTIO_KIND_9P,
    VIRTIO_KIND_INPUT,
} VirtioKindEnum;


/* One wrapper for every virtio device, on either transport. Which resources
   it needs is decided by the bus it was attached to: on MMIO it takes a
   register page and an interrupt line, on PCI it takes neither because the
   guest places the BARs and the bridge routes INTx. */
class VirtioDevice final: public Device {
private:
    VirtioKindEnum fKind;
    DeviceContext *fCtx;
    VMDeviceNode *fNode;
    VirtioInputTypeEnum fInputType = VIRTIO_INPUT_TYPE_KEYBOARD;
    const char *fTag = nullptr;
    Resource *fMmio = nullptr;
    Resource *fIrq = nullptr;
    VIRTIODevice *fDev = nullptr;

public:
    VirtioDevice(const char *name, VirtioKindEnum kind, DeviceContext *ctx,
                 VMDeviceNode *node):
        Device(name), fKind(kind), fCtx(ctx), fNode(node) {}

    void SetInputType(VirtioInputTypeEnum type) {fInputType = type;}
    void SetTag(const char *tag) {fTag = tag;}

    bool Prepare() override
    {
        if (ParentBus()->AsPCIBus() != nullptr) {
            return true;
        }
        fMmio = AddResource(RES_MMIO, VIRTIO_PAGE_SIZE, VIRTIO_PAGE_SIZE);
        fIrq = AddResource(RES_IRQ, 1);
        return fMmio != nullptr && fIrq != nullptr;
    }

    bool Realize() override
    {
        VIRTIOBusDef vbus = {};
        PCIBus *pci_bus = ParentBus()->AsPCIBus();

        if (pci_bus != nullptr) {
            vbus.pci_bus = pci_bus;
        } else {
            SystemBus *sys = static_cast<SystemBus *>(ParentBus());
            vbus.mem_map = sys->MemMap();
            vbus.addr = fMmio->base;
            vbus.irq = sys->IrqSignalFor(fIrq->base);
            if (vbus.irq == nullptr) {
                vm_error("%s: bad interrupt line %d\n", Name(),
                         (int)fIrq->base);
                return false;
            }
        }

        switch (fKind) {
        case VIRTIO_KIND_BLOCK:
            if (fNode->block_dev == nullptr) {
                vm_error("%s: no block back end\n", Name());
                return false;
            }
            fDev = virtio_block_init(&vbus, fNode->block_dev);
            break;
        case VIRTIO_KIND_NET:
            if (fNode->net == nullptr) {
                vm_error("%s: no network back end\n", Name());
                return false;
            }
            fDev = virtio_net_init(&vbus, fNode->net);
            fCtx->net = fNode->net;
            break;
        case VIRTIO_KIND_CONSOLE:
            if (fCtx->console == nullptr) {
                vm_error("%s: no console back end\n", Name());
                return false;
            }
            fDev = virtio_console_init(&vbus, fCtx->console);
            fCtx->console_dev = fDev;
            break;
        case VIRTIO_KIND_9P:
            if (fNode->fs_dev == nullptr) {
                vm_error("%s: no filesystem back end\n", Name());
                return false;
            }
            fDev = virtio_9p_init(&vbus, fNode->fs_dev, fTag);
            break;
        case VIRTIO_KIND_INPUT:
            fDev = virtio_input_init(&vbus, fInputType);
            if (fInputType == VIRTIO_INPUT_TYPE_KEYBOARD) {
                fCtx->keyboard_dev = fDev;
            } else {
                fCtx->mouse_dev = fDev;
            }
            break;
        }
        return fDev != nullptr;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        /* On PCI the guest finds the device by enumerating configuration
           space, so it must not also appear as a node. */
        if (fMmio == nullptr) {
            return;
        }
        ctx.fdt->BeginNodeNum("virtio", fMmio->base);
        ctx.fdt->PropStr("compatible", "virtio,mmio");
        ctx.fdt->PropU64Range("reg", fMmio->base, fMmio->size);
        fdt_prop_plic_irq(ctx, fIrq->base);
        ctx.fdt->EndNode();
    }
};


//#pragma mark - factory

static bool node_int_opt(const VMDeviceNode *node, const char *name, int *pval,
                         int def_val)
{
    return vm_get_int_opt(node->props, name, pval, def_val) >= 0;
}


Device *device_create(const VMDeviceNode *node, DeviceContext *ctx)
{
    const char *type = node->type;
    VMDeviceNode *mutable_node = const_cast<VMDeviceNode *>(node);

    if (strcmp(type, "ns16550a") == 0) {
        return new UartDevice(ctx, ctx->serial_output);
    }

    if (strcmp(type, "simplefb") == 0) {
        int width, height;
        if (vm_get_int(node->props, "width", &width) < 0 ||
            vm_get_int(node->props, "height", &height) < 0) {
            return nullptr;
        }
        return new SimpleFBDevice(ctx, width, height);
    }

    if (strcmp(type, "pci-host-ecam-generic") == 0) {
        int bus_count, mmio_size_mb;
        if (!node_int_opt(node, "bus_count", &bus_count,
                          PCIE_ECAM_DEFAULT_BUS_COUNT) ||
            !node_int_opt(node, "mmio_size", &mmio_size_mb,
                          PCIE_ECAM_DEFAULT_MMIO_SIZE >> 20)) {
            return nullptr;
        }
        return new PCIHostECAMDevice(node->id != nullptr ? node->id : "pcie",
                                     bus_count, (uint64_t)mmio_size_mb << 20);
    }

    if (strcmp(type, "pci-host-designware") == 0) {
        int mmio_size_mb;
        const char *compatible;
        if (!node_int_opt(node, "mmio_size", &mmio_size_mb,
                          PCIE_DW_DEFAULT_MMIO_SIZE >> 20)) {
            return nullptr;
        }
        /* Which controller this claims to be decides which driver binds to
           it, so it is worth setting from the configuration rather than
           being fixed here. */
        if (vm_get_str_opt(node->props, "compatible", &compatible) < 0) {
            return nullptr;
        }
        if (compatible == nullptr) {
            compatible = PCIE_DW_DEFAULT_COMPATIBLE;
        }
        return new PCIHostDWDevice(node->id != nullptr ? node->id : "pcie",
                                   compatible, (uint64_t)mmio_size_mb << 20);
    }

    if (strcmp(type, "nvme") == 0) {
        /* Deviations a guest needs are asked for by name, so that a
           conformant guest gets a conformant controller. */
        uint32_t quirks = 0;
        JSONValue list = json_object_get(node->props, "quirks");
        if (!json_is_undefined(list)) {
            if (list.type != JSON_ARRAY) {
                vm_error("nvme: 'quirks' must be an array of names\n");
                return nullptr;
            }
            for (int i = 0; i < list.u.array->len; i++) {
                JSONValue item = json_array_get(list, i);
                if (item.type != JSON_STR) {
                    vm_error("nvme: 'quirks' must be an array of names\n");
                    return nullptr;
                }
                uint32_t bits = nvme_quirks_from_name(item.u.str->data);
                if (bits == 0) {
                    vm_error("nvme: unknown quirk '%s'\n", item.u.str->data);
                    return nullptr;
                }
                quirks |= bits;
            }
        }
        if (node->children == nullptr) {
            vm_error("nvme: needs a nested NVMe bus with at least one "
                     "namespace on it\n");
            return nullptr;
        }
        return nvme_node_create(node->id != nullptr ? node->id : "nvme",
                                quirks);
    }

    if (strcmp(type, "nvme-ns") == 0) {
        int nsid;
        if (!node_int_opt(node, "nsid", &nsid, -1)) {
            return nullptr;
        }
        if (mutable_node->block_dev == nullptr) {
            vm_error("nvme-ns: no block back end\n");
            return nullptr;
        }
        return nvme_namespace_node_create(mutable_node->block_dev, nsid);
    }

    if (strcmp(type, "xhci") == 0) {
        int usb2_ports, usb3_ports;
        if (!node_int_opt(node, "usb2_ports", &usb2_ports,
                          XHCI_DEFAULT_USB2_PORTS) ||
            !node_int_opt(node, "usb3_ports", &usb3_ports,
                          XHCI_DEFAULT_USB3_PORTS)) {
            return nullptr;
        }
        return xhci_node_create(node->id != nullptr ? node->id : "xhci",
                                usb2_ports, usb3_ports);
    }

    if (strcmp(type, "usb-hub") == 0) {
        int ports, port;
        if (!node_int_opt(node, "ports", &ports, 4) ||
            !node_int_opt(node, "port", &port, 0)) {
            return nullptr;
        }
        if (ports < 1 || ports > USB_MAX_PORTS) {
            vm_error("usb-hub: 'ports' must be between 1 and %d\n",
                     USB_MAX_PORTS);
            return nullptr;
        }
        return usb_hub_node_create(ports, port);
    }

    if (strcmp(type, "usb-storage") == 0) {
        int port;
        if (!node_int_opt(node, "port", &port, 0)) {
            return nullptr;
        }
        if (node->children == nullptr) {
            vm_error("usb-storage: needs a nested SCSI bus with at least one "
                     "device on it\n");
            return nullptr;
        }
        return usb_storage_node_create(port);
    }

    if (strcmp(type, "scsi-disk") == 0) {
        int lun;
        if (!node_int_opt(node, "lun", &lun, -1)) {
            return nullptr;
        }
        if (mutable_node->block_dev == nullptr) {
            vm_error("scsi-disk: no block back end\n");
            return nullptr;
        }
        return scsi_disk_node_create(mutable_node->block_dev, lun);
    }

    if (strcmp(type, "virtio-block") == 0) {
        return new VirtioDevice("virtio-block", VIRTIO_KIND_BLOCK, ctx,
                                mutable_node);
    }

    if (strcmp(type, "virtio-net") == 0) {
        return new VirtioDevice("virtio-net", VIRTIO_KIND_NET, ctx,
                                mutable_node);
    }

    if (strcmp(type, "virtio-console") == 0) {
        return new VirtioDevice("virtio-console", VIRTIO_KIND_CONSOLE, ctx,
                                mutable_node);
    }

    if (strcmp(type, "virtio-9p") == 0) {
        const char *tag;
        if (vm_get_str(node->props, "tag", &tag) < 0) {
            return nullptr;
        }
        VirtioDevice *dev = new VirtioDevice("virtio-9p", VIRTIO_KIND_9P, ctx,
                                             mutable_node);
        dev->SetTag(tag);
        return dev;
    }

    if (strcmp(type, "virtio-input") == 0) {
        const char *kind;
        VirtioInputTypeEnum input_type;
        if (vm_get_str(node->props, "kind", &kind) < 0) {
            return nullptr;
        }
        if (strcmp(kind, "keyboard") == 0) {
            input_type = VIRTIO_INPUT_TYPE_KEYBOARD;
        } else if (strcmp(kind, "mouse") == 0) {
            input_type = VIRTIO_INPUT_TYPE_MOUSE;
        } else if (strcmp(kind, "tablet") == 0) {
            input_type = VIRTIO_INPUT_TYPE_TABLET;
        } else {
            vm_error("virtio-input: unsupported kind '%s'\n", kind);
            return nullptr;
        }
        VirtioDevice *dev = new VirtioDevice("virtio-input",
                                             VIRTIO_KIND_INPUT, ctx,
                                             mutable_node);
        dev->SetInputType(input_type);
        return dev;
    }

    vm_error("unsupported device type: %s\n", type);
    return nullptr;
}


bool device_build_tree(Bus *bus, VMDeviceNode *nodes, DeviceContext *ctx)
{
    for (VMDeviceNode *node = nodes; node != nullptr; node = node->next) {
        Device *dev = device_create(node, ctx);
        if (dev == nullptr) {
            return false;
        }
        if (!bus->AddDevice(dev)) {
            return false;
        }
        if (node->children != nullptr) {
            Bus *child = dev->ChildBus();
            if (child == nullptr) {
                vm_error("%s: device type '%s' does not provide a bus\n",
                         dev->Name(), node->type);
                return false;
            }
            /* The nesting in the file is the nesting of the buses, so a name
               that does not match the bus the device really provides is a
               mistake in the configuration rather than something to ignore. */
            if (node->child_bus_type != nullptr &&
                strcmp(node->child_bus_type, child->Type()) != 0) {
                vm_error("%s: device type '%s' provides a '%s' bus, but the "
                         "configuration declares a '%s' bus\n", dev->Name(),
                         node->type, child->Type(), node->child_bus_type);
                return false;
            }
            if (!device_build_tree(child, node->children, ctx)) {
                return false;
            }
        }
    }
    return true;
}
