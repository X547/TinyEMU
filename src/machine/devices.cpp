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
#include <string.h>

#include "ata.h"
#include "ata_pci.h"
#include "banshee.h"
#include "cutils.h"
#include "dw_i2c.h"
#include "dw_mmc.h"
#include "dwmac.h"
#include "hda.h"
#include "hda_codec.h"
#include "hid.h"
#include "intel_hda.h"
#include "mdio.h"
#include "ne2000.h"
#include "nvme.h"
#include "pci_bridge.h"
#include "pci_host_dw.h"
#include "pci_host_plda.h"
#include "pci_host_ecam.h"
#include "ps2.h"
#include "scsi.h"
#include "sd.h"
#include "sdhci.h"
#include "simplefb.h"
#include "syscon_poweroff.h"
#include "usb.h"
#include "xhci.h"

/* The PC's own parts. They are built only with the x86 machine, because none
   of them models anything a device tree machine has. */
#ifdef CONFIG_X86EMU
#include "hpet.h"
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


/* The node's "file", relative to the configuration file, opened as a disk
   image. Reports and returns nullptr on failure. */
static std::unique_ptr<HostBlockDevice> node_open_block(const VMDeviceNode *node,
                                                        DeviceContext *ctx)
{
    std::unique_ptr<HostBlockDevice> bs;
    char *fname;

    if (node->filename.empty()) {
        vm_error("%s: expecting a 'file' property\n", node->type.c_str());
        return nullptr;
    }
    fname = get_file_path(ctx->params->cfg_filename, node->filename.c_str());
    bs = ctx->platform->OpenBlockDevice(fname);
    free(fname);
    if (bs == nullptr) {
        vm_error("%s: could not open\n", node->filename.c_str());
    }
    return bs;
}


/* A dotted quad, as a host byte order value. Parsed here rather than by the
   host's resolver so that the configuration loader needs no socket
   headers. */
static bool parse_ipv4(const char *str, uint32_t *out)
{
    uint32_t addr = 0;

    for (int i = 0; i < 4; i++) {
        if (i > 0 && *str++ != '.') {
            return false;
        }
        uint32_t octet = 0;
        int digits = 0;
        while (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (uint32_t)(*str++ - '0');
            if (++digits > 3 || octet > 255) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        addr = (addr << 8) | octet;
    }
    if (*str != '\0') {
        return false;
    }
    *out = addr;
    return true;
}


/* One "forward" entry: a host port, and the guest port a connection to it
   reaches. Reports and returns false on anything it cannot read. */
static bool node_parse_forward(const char *type, JSONValue item,
                               EthernetForward *out)
{
    const char *proto, *host_addr, *guest_addr;
    int host_port, guest_port;

    if (item.type != JSON_OBJ) {
        vm_error("%s: every 'forward' entry must be an object\n", type);
        return false;
    }
    if (vm_get_str_opt(item, "proto", &proto) < 0 ||
        vm_get_str_opt(item, "host_addr", &host_addr) < 0 ||
        vm_get_str_opt(item, "guest_addr", &guest_addr) < 0 ||
        vm_get_int(item, "host_port", &host_port) < 0 ||
        vm_get_int(item, "guest_port", &guest_port) < 0) {
        return false;
    }

    if (proto != nullptr && strcmp(proto, "udp") == 0) {
        out->is_udp = true;
    } else if (proto != nullptr && strcmp(proto, "tcp") != 0) {
        vm_error("%s: 'proto' must be \"tcp\" or \"udp\"\n", type);
        return false;
    }

    /* Only the host itself reaches the guest unless the configuration says
       otherwise: a forward is a hole through to a guest, and one opened on
       every address of the host offers it to the whole network. */
    out->host_addr = 0x7f000001;
    if (host_addr != nullptr && !parse_ipv4(host_addr, &out->host_addr)) {
        vm_error("%s: 'host_addr' is not an address\n", type);
        return false;
    }
    /* Left at zero, the back end uses whichever address it gave the guest. */
    if (guest_addr != nullptr && !parse_ipv4(guest_addr, &out->guest_addr)) {
        vm_error("%s: 'guest_addr' is not an address\n", type);
        return false;
    }

    if (host_port < 1 || host_port > 65535 ||
        guest_port < 1 || guest_port > 65535) {
        vm_error("%s: 'host_port' and 'guest_port' must be between 1 and "
                 "65535\n", type);
        return false;
    }
    out->host_port = host_port;
    out->guest_port = guest_port;
    return true;
}


/* The network back end the node's "driver" names. Reports and returns
   nullptr on failure. */
static std::unique_ptr<HostEthernet> node_open_ethernet(const VMDeviceNode *node,
                                                        DeviceContext *ctx)
{
    const char *type = node->type.c_str();
    const char *driver, *ifname;
    std::vector<EthernetForward> forwards;

    if (vm_get_str(node->props, "driver", &driver) < 0 ||
        vm_get_str_opt(node->props, "ifname", &ifname) < 0) {
        return nullptr;
    }

    JSONValue list = json_object_get(node->props, "forward");
    if (!json_is_undefined(list)) {
        if (list.type != JSON_ARRAY) {
            vm_error("%s: 'forward' must be an array of entries\n", type);
            return nullptr;
        }
        for (int i = 0; i < list.u.array->Length(); i++) {
            EthernetForward fwd;
            if (!node_parse_forward(type, json_array_get(list, i), &fwd)) {
                return nullptr;
            }
            forwards.push_back(fwd);
        }
    }

    return ctx->platform->OpenEthernet(driver, ifname, forwards);
}


/* The node's "file", relative to the configuration file, opened as a
   directory tree to share. Reports and returns nullptr on failure. */
static std::unique_ptr<HostFileSystem> node_open_fs(const VMDeviceNode *node,
                                                    DeviceContext *ctx)
{
    std::unique_ptr<HostFileSystem> fs;
    char *fname;

    if (node->filename.empty()) {
        vm_error("%s: expecting a 'file' property\n", node->type.c_str());
        return nullptr;
    }
    fname = get_file_path(ctx->params->cfg_filename, node->filename.c_str());
    fs = ctx->platform->OpenFileSystem(fname);
    free(fname);
    return fs;
}


/* The node's "host" object: where a port's sound goes to or comes from.
   Without one it is the host's own audio system. Reports and returns nullptr
   on failure. */
static std::unique_ptr<HostAudio> node_open_audio(const VMDeviceNode *node,
                                                  DeviceContext *ctx,
                                                  AudioDirectionEnum direction)
{
    const char *type = node->type.c_str();
    AudioSettings settings;
    const char *driver = nullptr, *device = nullptr, *file = nullptr;
    int loop = 0, latency = settings.latency_ms;

    settings.direction = direction;
    JSONValue host = json_object_get(node->props, "host");
    if (!json_is_undefined(host)) {
        if (host.type != JSON_OBJ) {
            vm_error("%s: 'host' must be an object\n", type);
            return nullptr;
        }
        if (vm_get_str_opt(host, "driver", &driver) < 0 ||
            vm_get_str_opt(host, "device", &device) < 0 ||
            vm_get_str_opt(host, "file", &file) < 0 ||
            vm_get_int_opt(host, "loop", &loop, 0) < 0 ||
            vm_get_int_opt(host, "latency", &latency, latency) < 0) {
            return nullptr;
        }
    }
    if (latency < 1 || latency > 1000) {
        vm_error("%s: 'latency' must be between 1 and 1000 ms\n", type);
        return nullptr;
    }
    if (driver != nullptr) {
        settings.driver = driver;
    }
    settings.device = device;
    settings.loop = loop != 0;
    settings.latency_ms = latency;

    char *path = nullptr;
    if (file != nullptr) {
        path = get_file_path(ctx->params->cfg_filename, file);
        settings.file = path;
    }
    auto audio = ctx->platform->OpenAudio(settings);
    free(path);
    return audio;
}


/* An "hda-output" or "hda-input" node's pin and converter. Reports and
   returns false on anything it cannot read. */
static bool node_parse_hda_port(const VMDeviceNode *node, HDAPortConfig *out)
{
    const char *type = node->type.c_str();
    const char *kind, *location;
    int plugged;

    if (vm_get_str_opt(node->props, "kind", &kind) < 0 ||
        vm_get_str_opt(node->props, "location", &location) < 0 ||
        !node_int_opt(node, "association", &out->association, 1) ||
        !node_int_opt(node, "sequence", &out->sequence, 0) ||
        !node_int_opt(node, "channels", &out->channels, 2) ||
        !node_int_opt(node, "plugged", &plugged, 1)) {
        return false;
    }
    if (kind != nullptr && !hda_pin_kind_from_name(kind, &out->kind)) {
        vm_error("%s: unknown kind '%s'\n", type, kind);
        return false;
    }
    if (location != nullptr &&
        !hda_pin_location_from_name(location, &out->location)) {
        vm_error("%s: unknown location '%s'\n", type, location);
        return false;
    }
    if (out->association < 1 || out->association > 15) {
        vm_error("%s: 'association' must be between 1 and 15\n", type);
        return false;
    }
    if (out->sequence < 0 || out->sequence > 15) {
        vm_error("%s: 'sequence' must be between 0 and 15\n", type);
        return false;
    }
    if (out->channels < 1 || out->channels > 16) {
        vm_error("%s: 'channels' must be between 1 and 16\n", type);
        return false;
    }
    out->plugged = plugged != 0;

    JSONValue list = json_object_get(node->props, "rates");
    if (!json_is_undefined(list)) {
        if (list.type != JSON_ARRAY) {
            vm_error("%s: 'rates' must be an array of sample rates\n", type);
            return false;
        }
        for (int i = 0; i < list.u.array->Length(); i++) {
            JSONValue item = json_array_get(list, i);
            if (item.type != JSON_INT || hda_rate_index(item.u.int32) < 0) {
                vm_error("%s: 'rates' may only hold rates the link carries, "
                         "8000 to 192000\n", type);
                return false;
            }
            out->rates.push_back(item.u.int32);
        }
    }
    return true;
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

    if (strcmp(type, "ns16550a") == 0) {
        int port, irq;
        if (!node_int_opt(node, "reg", &port, -1) ||
            !node_int_opt(node, "irq", &irq, -1)) {
            return nullptr;
        }
        return uart_node_create(ctx, port, irq);
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

    if (strcmp(type, "banshee") == 0) {
        const char *model;
        int vram_mb, width, height;
        if (vm_get_str_opt(node->props, "model", &model) < 0 ||
            !node_int_opt(node, "vram", &vram_mb, BANSHEE_DEFAULT_VRAM_MB) ||
            !node_int_opt(node, "width", &width, 1024) ||
            !node_int_opt(node, "height", &height, 768)) {
            return nullptr;
        }
        return banshee_node_create(ctx, node->IdOr("banshee"), model, vram_mb,
                                   width, height);
    }

    if (strcmp(type, "syscon-poweroff") == 0) {
        return syscon_poweroff_node_create(ctx);
    }

#ifdef CONFIG_X86EMU
    if (strcmp(type, "i8042") == 0) {
        int vmmouse;
        if (!node_int_opt(node, "vmmouse", &vmmouse, 1)) {
            return nullptr;
        }
        return i8042_node_create(ctx, vmmouse != 0);
    }

    if (strcmp(type, "pci-host-i440fx") == 0 ||
        strcmp(type, "pci-host-cloudhv") == 0) {
        if (ctx->pc_pci_space == nullptr) {
            vm_error("%s: only the pc machine has one\n", type);
            return nullptr;
        }
        I440FXVariant variant = strcmp(type, "pci-host-cloudhv") == 0 ?
            I440FX_CLOUD_HYPERVISOR : I440FX_PC;
        return i440fx_node_create(node->IdOr("i440fx"), variant,
                                  *ctx->pc_pci_space);
    }

    if (strcmp(type, "hpet") == 0) {
        return hpet_node_create(ctx);
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
        auto bs = node_open_block(node, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        return ata_disk_node_create(std::move(bs), read_only != 0);
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

    if (strcmp(type, "pci-host-plda") == 0) {
        int mmio_size_mb, mmio64_size_mb, bus_count;
        if (!node_int_opt(node, "mmio_size", &mmio_size_mb,
                          PCIE_PLDA_DEFAULT_MMIO_SIZE >> 20) ||
            !node_int_opt(node, "mmio64_size", &mmio64_size_mb,
                          PCIE_PLDA_DEFAULT_MMIO64_SIZE >> 20) ||
            !node_int_opt(node, "bus_count", &bus_count,
                          PCIE_PLDA_DEFAULT_BUS_COUNT)) {
            return nullptr;
        }
        return new PCIHostPLDADevice(node->IdOr("pcie"),
                                     (uint64_t)mmio_size_mb << 20,
                                     (uint64_t)mmio64_size_mb << 20,
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
        int nsid, read_only;
        if (!node_int_opt(node, "nsid", &nsid, -1) ||
            !node_int_opt(node, "read_only", &read_only, 0)) {
            return nullptr;
        }
        auto bs = node_open_block(node, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        return nvme_namespace_node_create(std::move(bs), nsid, read_only != 0);
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

    if (strcmp(type, "dw-mmc") == 0) {
        const char *compatible;
        int clock_mhz, dma;
        if (vm_get_str_opt(node->props, "compatible", &compatible) < 0 ||
            !node_int_opt(node, "clock", &clock_mhz,
                          DW_MMC_DEFAULT_CLOCK_HZ / 1000000) ||
            !node_int_opt(node, "dma", &dma, DW_MMC_DEFAULT_DMA_BITS)) {
            return nullptr;
        }
        if (compatible == nullptr) {
            compatible = DW_MMC_DEFAULT_COMPATIBLE;
        }
        if (clock_mhz < 1 || clock_mhz > 1000) {
            vm_error("dw-mmc: 'clock' must be between 1 and 1000 MHz\n");
            return nullptr;
        }
        if (dma != 0 && dma != 32 && dma != 64) {
            vm_error("dw-mmc: 'dma' must be 0, 32 or 64\n");
            return nullptr;
        }
        return dw_mmc_node_create(node->IdOr("dw-mmc"), compatible,
                                  (uint32_t)clock_mhz * 1000000, dma);
    }

    if (strcmp(type, "dw-i2c") == 0) {
        const char *compatible;
        if (vm_get_str_opt(node->props, "compatible", &compatible) < 0) {
            return nullptr;
        }
        if (compatible == nullptr) {
            compatible = DW_I2C_DEFAULT_COMPATIBLE;
        }
        return dw_i2c_node_create(node->IdOr("dw-i2c"), compatible);
    }

    if (strcmp(type, "sd-card") == 0 || strcmp(type, "mmc-card") == 0) {
        int read_only;
        if (!node_int_opt(node, "read_only", &read_only, 0)) {
            return nullptr;
        }
        auto bs = node_open_block(node, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        if (strcmp(type, "sd-card") == 0) {
            return sd_card_node_create(std::move(bs), read_only != 0);
        }
        return mmc_card_node_create(std::move(bs), read_only != 0);
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

    if (strcmp(type, "intel-hda") == 0) {
        int input_streams, output_streams;
        if (!node_int_opt(node, "input_streams", &input_streams,
                          INTEL_HDA_DEFAULT_STREAMS) ||
            !node_int_opt(node, "output_streams", &output_streams,
                          INTEL_HDA_DEFAULT_STREAMS)) {
            return nullptr;
        }
        return intel_hda_node_create(node->IdOr("hda"), input_streams,
                                     output_streams);
    }

    if (strcmp(type, "hda-codec") == 0) {
        int address, vendor_id, subsystem_id, revision_id;
        if (!node_int_opt(node, "address", &address, -1) ||
            !node_int_opt(node, "vendor_id", &vendor_id,
                          HDA_CODEC_DEFAULT_VENDOR_ID) ||
            !node_int_opt(node, "subsystem_id", &subsystem_id, 0) ||
            !node_int_opt(node, "revision_id", &revision_id, 0x00100100)) {
            return nullptr;
        }
        if (node->children == nullptr) {
            vm_error("hda-codec: needs a nested bus with a function group on "
                     "it\n");
            return nullptr;
        }
        return hda_codec_node_create(node->IdOr("hda-codec"), address,
                                     vendor_id, subsystem_id, revision_id);
    }

    if (strcmp(type, "hda-audio-group") == 0) {
        return hda_audio_group_node_create(node->IdOr("hda-audio-group"));
    }

    if (strcmp(type, "hda-output") == 0) {
        HDAPortConfig config;
        if (!node_parse_hda_port(node, &config)) {
            return nullptr;
        }
        auto audio = node_open_audio(node, ctx, AUDIO_RENDER);
        if (audio == nullptr) {
            return nullptr;
        }
        return hda_output_node_create(node->IdOr("hda-output"), config,
                                      std::move(audio));
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

    if (strcmp(type, "i2c-hid") == 0) {
        int address;
        /* Without an address the bus places the target, as it does an
           Ethernet PHY. */
        if (!node_int_opt(node, "reg", &address, -1)) {
            return nullptr;
        }
        if (node->children == nullptr) {
            vm_error("i2c-hid: needs a nested HID bus with a function on "
                     "it\n");
            return nullptr;
        }
        return i2c_hid_node_create(address);
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
        int target, lun, read_only;
        if (!node_int_opt(node, "target", &target, -1) ||
            !node_int_opt(node, "lun", &lun, -1) ||
            !node_int_opt(node, "read_only", &read_only, 0)) {
            return nullptr;
        }
        auto bs = node_open_block(node, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        return scsi_disk_node_create(std::move(bs), target, lun,
                                     read_only != 0);
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
        auto net = node_open_ethernet(node, ctx);
        if (net == nullptr) {
            return nullptr;
        }
        return new DwmacDevice(ctx, std::move(net), compatible, phy_mode,
                               quirks);
    }

    if (strcmp(type, "ne2000") == 0) {
        int port, irq;
        /* Only a card jumpered onto a port based machine names either; on
           PCI the guest places both. */
        if (!node_int_opt(node, "reg", &port, -1) ||
            !node_int_opt(node, "irq", &irq, -1)) {
            return nullptr;
        }
        auto net = node_open_ethernet(node, ctx);
        if (net == nullptr) {
            return nullptr;
        }
        return new NE2000Device(ctx, std::move(net), node->IdOr("ne2000"),
                                port, irq);
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
        int read_only;
        if (!node_int_opt(node, "read_only", &read_only, 0)) {
            return nullptr;
        }
        auto bs = node_open_block(node, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        return virtio_block_node_create(ctx, std::move(bs), read_only != 0);
    }

    if (strcmp(type, "virtio-scsi") == 0) {
        return virtio_scsi_node_create(ctx);
    }

    if (strcmp(type, "virtio-net") == 0) {
        auto net = node_open_ethernet(node, ctx);
        if (net == nullptr) {
            return nullptr;
        }
        return virtio_net_node_create(ctx, std::move(net));
    }

    if (strcmp(type, "virtio-console") == 0) {
        return virtio_console_node_create(ctx);
    }

    if (strcmp(type, "virtio-9p") == 0) {
        const char *tag;
        if (vm_get_str(node->props, "tag", &tag) < 0) {
            return nullptr;
        }
        auto fs = node_open_fs(node, ctx);
        if (fs == nullptr) {
            return nullptr;
        }
        return virtio_9p_node_create(ctx, std::move(fs), tag);
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
        return virtio_input_node_create(ctx, input_type);
    }

    if (strcmp(type, "virtio-gpu") == 0) {
        int width, height;
        if (!node_int_opt(node, "width", &width, 1024) ||
            !node_int_opt(node, "height", &height, 768)) {
            return nullptr;
        }
        if (width < VIRTIO_GPU_MIN_SIZE || width > VIRTIO_GPU_MAX_SIZE ||
            height < VIRTIO_GPU_MIN_SIZE || height > VIRTIO_GPU_MAX_SIZE) {
            vm_error("virtio-gpu: 'width' and 'height' must be between %d "
                     "and %d\n", VIRTIO_GPU_MIN_SIZE, VIRTIO_GPU_MAX_SIZE);
            return nullptr;
        }
        return virtio_gpu_node_create(ctx, width, height);
    }

    if (strcmp(type, "ps2-keyboard") == 0 || strcmp(type, "ps2-mouse") == 0) {
        int port;
        if (!node_int_opt(node, "port", &port, -1)) {
            return nullptr;
        }
        if (strcmp(type, "ps2-keyboard") == 0) {
            return ps2_keyboard_node_create(port);
        }
        return ps2_mouse_node_create(port);
    }

    if (strcmp(type, "ps2") == 0) {
        vm_error("'ps2' is now an 'i8042' controller carrying a "
                 "'ps2-keyboard' and a 'ps2-mouse' on the PS/2 bus it "
                 "provides\n");
        return nullptr;
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


void device_context_connect(DeviceContext *ctx)
{
    Platform *platform = ctx->platform;
    HostConsole *console = platform->Console();

    if (console != nullptr) {
        console->SetTarget(ctx->console_input != nullptr ? ctx->console_input
                                                         : ctx->serial_input);
    }
    HostScreen *screen = platform->Screen();
    if (screen != nullptr && ctx->screen != nullptr) {
        screen->SetSource(ctx->screen, ctx->screen_width, ctx->screen_height);
        ctx->screen->SetScreen(screen);
        /* only a frame buffer the guest writes directly has to be polled */
        FBDevice *fb = dynamic_cast<FBDevice *>(ctx->screen);
        if (fb != nullptr) {
            ctx->machine->SetDisplay(screen, fb);
        }
    }
    if (platform->Keyboard() != nullptr) {
        platform->Keyboard()->SetTarget(ctx->keyboard);
    }
    if (platform->Pointer() != nullptr) {
        platform->Pointer()->SetTarget(ctx->mouse);
    }
    for (HostEthernet *net : ctx->ethernet) {
        if (net->target != nullptr) {
            net->target->SetCarrier(true);
        }
    }
}
