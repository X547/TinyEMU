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

#include <string.h>

#include "ata.h"
#include "ata_pci.h"
#include "cutils.h"
#include "dwmac.h"
#include "hid.h"
#include "mdio.h"
#include "nvme.h"
#include "pci_bridge.h"
#include "pci_host_dw.h"
#include "pci_host_ecam.h"
#include "scsi.h"
#include "sd.h"
#include "sdhci.h"
#include "simplefb.h"
#include "usb.h"
#include "xhci.h"

/* The PC's own parts. They are built only with the x86 machine, because none
   of them models anything a device tree machine has. */
#ifdef CONFIG_X86EMU
#include "i8042.h"
#include "pci_host_i440fx.h"
#include "vga.h"
#endif


//#pragma mark - factory

static bool node_int_opt(const VMDeviceNode *node, const char *name, int *pval,
                         int def_val)
{
    return vm_get_int_opt(node->props, name, pval, def_val) >= 0;
}


/* An "io_size" in KB as a byte count. A PCI to PCI bridge forwards I/O in
   4 KB units and the window registers hold a power of two, so an aperture
   that is neither is one no guest could place devices in. 0 asks for a host
   bridge with no I/O aperture at all, which is what every machine had before
   there was one. */
static bool pci_host_io_size(const char *type, int size_kb, uint64_t *out)
{
    if (size_kb == 0) {
        *out = 0;
        return true;
    }
    if (size_kb < 4 || size_kb > 65536 ||
        (size_kb & (size_kb - 1)) != 0) {
        vm_error("%s: 'io_size' must be 0 or a power of two between 4 and "
                 "65536 KB\n", type);
        return false;
    }
    *out = (uint64_t)size_kb << 10;
    return true;
}


Device *device_create(const VMDeviceNode *node, DeviceContext *ctx)
{
    const char *type = node->type.c_str();
    VMDeviceNode *mutable_node = const_cast<VMDeviceNode *>(node);

    if (strcmp(type, "ns16550a") == 0) {
        int port, irq;
        if (!node_int_opt(node, "reg", &port, -1) ||
            !node_int_opt(node, "irq", &irq, -1)) {
            return nullptr;
        }
        return uart_node_create(ctx, ctx->serial_output, port, irq);
    }

    if (strcmp(type, "simplefb") == 0 || strcmp(type, "vga") == 0) {
        int width, height;
        if (vm_get_int(node->props, "width", &width) < 0 ||
            vm_get_int(node->props, "height", &height) < 0) {
            return nullptr;
        }
#ifdef CONFIG_X86EMU
        if (strcmp(type, "vga") == 0) {
            return vga_node_create(ctx, width, height);
        }
#endif
        return simplefb_node_create(ctx, width, height);
    }

#ifdef CONFIG_X86EMU
    if (strcmp(type, "ps2") == 0) {
        int vmmouse;
        if (!node_int_opt(node, "vmmouse", &vmmouse, 1)) {
            return nullptr;
        }
        return i8042_node_create(ctx, vmmouse != 0);
    }

    if (strcmp(type, "pci-host-i440fx") == 0) {
        return i440fx_node_create(node->IdOr("i440fx"));
    }
#endif

    if (strcmp(type, "pci-ide") == 0) {
        if (node->children == nullptr) {
            vm_error("pci-ide: needs a nested ATA bus with at least one "
                     "drive on it\n");
            return nullptr;
        }
        return ata_pci_node_create(node->IdOr("ide"));
    }

    if (strcmp(type, "ata-disk") == 0) {
        int read_only;
        if (!node_int_opt(node, "read_only", &read_only, 0)) {
            return nullptr;
        }
        if (mutable_node->block_dev == nullptr) {
            vm_error("ata-disk: no block back end\n");
            return nullptr;
        }
        return ata_disk_node_create(std::move(mutable_node->block_dev),
                                    read_only != 0);
    }

    if (strcmp(type, "pci-host-ecam-generic") == 0) {
        int bus_count, mmio_size_mb, mmio64_size_mb, io_size_kb;
        if (!node_int_opt(node, "bus_count", &bus_count,
                          PCIE_ECAM_DEFAULT_BUS_COUNT) ||
            !node_int_opt(node, "mmio_size", &mmio_size_mb,
                          PCIE_ECAM_DEFAULT_MMIO_SIZE >> 20) ||
            !node_int_opt(node, "mmio64_size", &mmio64_size_mb,
                          PCIE_ECAM_DEFAULT_MMIO64_SIZE >> 20) ||
            !node_int_opt(node, "io_size", &io_size_kb,
                          PCIE_ECAM_DEFAULT_IO_SIZE >> 10)) {
            return nullptr;
        }
        uint64_t io_size;
        if (!pci_host_io_size(type, io_size_kb, &io_size)) {
            return nullptr;
        }
        return new PCIHostECAMDevice(node->IdOr("pcie"),
                                     bus_count, (uint64_t)mmio_size_mb << 20,
                                     (uint64_t)mmio64_size_mb << 20, io_size);
    }

    if (strcmp(type, "pci-host-designware") == 0) {
        int mmio_size_mb, mmio64_size_mb, io_size_kb, bus_count;
        const char *compatible;
        if (!node_int_opt(node, "mmio_size", &mmio_size_mb,
                          PCIE_DW_DEFAULT_MMIO_SIZE >> 20) ||
            !node_int_opt(node, "mmio64_size", &mmio64_size_mb,
                          PCIE_DW_DEFAULT_MMIO64_SIZE >> 20) ||
            !node_int_opt(node, "io_size", &io_size_kb,
                          PCIE_DW_DEFAULT_IO_SIZE >> 10) ||
            !node_int_opt(node, "bus_count", &bus_count,
                          PCIE_DW_DEFAULT_BUS_COUNT)) {
            return nullptr;
        }
        uint64_t io_size;
        if (!pci_host_io_size(type, io_size_kb, &io_size)) {
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
        return new PCIHostDWDevice(node->IdOr("pcie"),
                                   compatible, (uint64_t)mmio_size_mb << 20,
                                   (uint64_t)mmio64_size_mb << 20, io_size,
                                   bus_count);
    }

    if (strcmp(type, "pci-bridge") == 0) {
        return pci_bridge_node_create(node->IdOr("pci-bridge"));
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
            for (int i = 0; i < list.u.array->Length(); i++) {
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
        return nvme_node_create(node->IdOr("nvme"),
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
        return nvme_namespace_node_create(std::move(mutable_node->block_dev),
                                          nsid);
    }

    if (strcmp(type, "sdhci") == 0) {
        const char *compatible;
        int clock_mhz;
        /* On a device tree machine the node has to name a controller some
           driver binds to, exactly as the DesignWare host bridge does. On
           PCI the class code does that job and the property is unused. */
        if (vm_get_str_opt(node->props, "compatible", &compatible) < 0 ||
            !node_int_opt(node, "clock", &clock_mhz,
                          SDHCI_DEFAULT_CLOCK_HZ / 1000000)) {
            return nullptr;
        }
        if (compatible == nullptr) {
            compatible = SDHCI_DEFAULT_COMPATIBLE;
        }
        if (clock_mhz < 1 || clock_mhz > 255) {
            vm_error("sdhci: 'clock' must be between 1 and 255 MHz\n");
            return nullptr;
        }
        return sdhci_node_create(node->IdOr("sdhci"),
                                 compatible, (uint32_t)clock_mhz * 1000000);
    }

    if (strcmp(type, "sd-card") == 0 || strcmp(type, "mmc-card") == 0) {
        int read_only;
        if (!node_int_opt(node, "read_only", &read_only, 0)) {
            return nullptr;
        }
        if (mutable_node->block_dev == nullptr) {
            vm_error("%s: no block back end\n", type);
            return nullptr;
        }
        if (strcmp(type, "sd-card") == 0) {
            return sd_card_node_create(std::move(mutable_node->block_dev),
                                       read_only != 0);
        }
        return mmc_card_node_create(std::move(mutable_node->block_dev),
                                    read_only != 0);
    }

    if (strcmp(type, "xhci") == 0) {
        int usb2_ports, usb3_ports;
        if (!node_int_opt(node, "usb2_ports", &usb2_ports,
                          XHCI_DEFAULT_USB2_PORTS) ||
            !node_int_opt(node, "usb3_ports", &usb3_ports,
                          XHCI_DEFAULT_USB3_PORTS)) {
            return nullptr;
        }
        return xhci_node_create(node->IdOr("xhci"),
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

    if (strcmp(type, "usb-hid") == 0) {
        int port;
        if (!node_int_opt(node, "port", &port, 0)) {
            return nullptr;
        }
        if (node->children == nullptr) {
            vm_error("usb-hid: needs a nested HID bus with at least one "
                     "function on it\n");
            return nullptr;
        }
        return usb_hid_node_create(port);
    }

    if (strcmp(type, "hid-keyboard") == 0 || strcmp(type, "hid-tablet") == 0) {
        int index;
        if (!node_int_opt(node, "index", &index, -1)) {
            return nullptr;
        }
        if (strcmp(type, "hid-keyboard") == 0) {
            return hid_keyboard_node_create(ctx, index);
        }
        return hid_tablet_node_create(ctx, index);
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
        return scsi_disk_node_create(std::move(mutable_node->block_dev), lun);
    }

    if (strcmp(type, "dwmac") == 0) {
        const char *compatible, *phy_mode;
        /* Which controller this claims to be decides which driver binds to
           it, so it is worth setting from the configuration rather than
           being fixed here. */
        if (vm_get_str_opt(node->props, "compatible", &compatible) < 0 ||
            vm_get_str_opt(node->props, "phy_mode", &phy_mode) < 0) {
            return nullptr;
        }
        if (compatible == nullptr) {
            compatible = DWMAC_DEFAULT_COMPATIBLE;
        }
        if (phy_mode == nullptr) {
            phy_mode = DWMAC_DEFAULT_PHY_MODE;
        }
        /* Deviations a guest needs are asked for by name, so that a
           conformant driver gets a conformant MAC. */
        uint32_t quirks = 0;
        JSONValue list = json_object_get(node->props, "quirks");
        if (!json_is_undefined(list)) {
            if (list.type != JSON_ARRAY) {
                vm_error("dwmac: 'quirks' must be an array of names\n");
                return nullptr;
            }
            for (int i = 0; i < list.u.array->Length(); i++) {
                JSONValue item = json_array_get(list, i);
                if (item.type != JSON_STR) {
                    vm_error("dwmac: 'quirks' must be an array of names\n");
                    return nullptr;
                }
                uint32_t bits = dwmac_quirks_from_name(item.u.str->data);
                if (bits == 0) {
                    vm_error("dwmac: unknown quirk '%s'\n", item.u.str->data);
                    return nullptr;
                }
                quirks |= bits;
            }
        }
        return new DwmacDevice(ctx, mutable_node, compatible, phy_mode,
                               quirks);
    }

    if (strcmp(type, "ethernet-phy") == 0) {
        int address, phy_id;
        /* Without an address the bus places the PHY, exactly as the MMIO
           allocator places a device that names no base. */
        if (!node_int_opt(node, "reg", &address, -1) ||
            !node_int_opt(node, "phy_id", &phy_id, PHY_GENERIC_ID)) {
            return nullptr;
        }
        return new PHYDevice(address, (uint32_t)phy_id);
    }

    if (strcmp(type, "virtio-block") == 0) {
        return virtio_block_node_create(ctx, mutable_node);
    }

    if (strcmp(type, "virtio-net") == 0) {
        return virtio_net_node_create(ctx, mutable_node);
    }

    if (strcmp(type, "virtio-console") == 0) {
        return virtio_console_node_create(ctx, mutable_node);
    }

    if (strcmp(type, "virtio-9p") == 0) {
        const char *tag;
        if (vm_get_str(node->props, "tag", &tag) < 0) {
            return nullptr;
        }
        return virtio_9p_node_create(ctx, mutable_node, tag);
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
        return virtio_input_node_create(ctx, mutable_node, input_type);
    }

    if (strcmp(type, "ide") == 0) {
        vm_error("'ide' is now a 'pci-ide' controller carrying an 'ata-disk' "
                 "on the ATA bus it provides, declared inside the PCI bus of "
                 "a host bridge\n");
        return nullptr;
    }

    vm_error("unsupported device type: %s\n", type);
    return nullptr;
}


bool device_build_tree(Bus *bus, VMDeviceNode *nodes, DeviceContext *ctx)
{
    for (VMDeviceNode *node = nodes; node != nullptr; node = node->next.get()) {
        std::unique_ptr<Device> owned(device_create(node, ctx));
        Device *dev = owned.get();
        if (dev == nullptr) {
            return false;
        }
        if (!bus->AddDevice(std::move(owned))) {
            return false;
        }
        if (node->children != nullptr) {
            Bus *child = dev->ChildBus();
            if (child == nullptr) {
                vm_error("%s: device type '%s' does not provide a bus\n",
                         dev->Name(), node->type.c_str());
                return false;
            }
            /* The nesting in the file is the nesting of the buses, so a name
               that does not match the bus the device really provides is a
               mistake in the configuration rather than something to ignore. */
            if (!node->child_bus_type.empty() &&
                node->child_bus_type != child->Type()) {
                vm_error("%s: device type '%s' provides a '%s' bus, but the "
                         "configuration declares a '%s' bus\n", dev->Name(),
                         node->type.c_str(), child->Type(),
                         node->child_bus_type.c_str());
                return false;
            }
            if (!device_build_tree(child, node->children.get(), ctx)) {
                return false;
            }
        }
    }
    return true;
}
