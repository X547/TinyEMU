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
#include "scsi.h"
#include "sd.h"
#include "sdhci.h"
#include "usb.h"
#include "xhci.h"


//#pragma mark - factory

/* The node's "file", relative to the configuration file, opened as a disk
   image. Reports and returns nullptr on failure. */
static std::unique_ptr<HostBlockDevice> node_open_block(const DeviceConfig &cfg,
                                                        DeviceContext *ctx,
                                                        bool read_only = false)
{
    std::unique_ptr<HostBlockDevice> bs;
    const char *file;
    char *fname;

    if (!cfg.GetStrOpt("file", &file)) {
        return nullptr;
    }
    if (file == nullptr) {
        vm_error("%s: expecting a 'file' property\n", cfg.Type());
        return nullptr;
    }
    fname = cfg.ResolvePath(file);
    bs = ctx->platform->OpenBlockDevice(fname, read_only);
    free(fname);
    if (bs == nullptr) {
        vm_error("%s: could not open\n", file);
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
static std::unique_ptr<HostEthernet> node_open_ethernet(const DeviceConfig &cfg,
                                                        DeviceContext *ctx)
{
    const char *type = cfg.Type();
    const char *driver, *ifname;
    std::vector<EthernetForward> forwards;

    if (!cfg.GetStr("driver", &driver) ||
        !cfg.GetStrOpt("ifname", &ifname)) {
        return nullptr;
    }

    JSONValue list = cfg.Get("forward");
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
static std::unique_ptr<HostFileSystem> node_open_fs(const DeviceConfig &cfg,
                                                    DeviceContext *ctx)
{
    std::unique_ptr<HostFileSystem> fs;
    const char *file;
    char *fname;

    if (!cfg.GetStrOpt("file", &file)) {
        return nullptr;
    }
    if (file == nullptr) {
        vm_error("%s: expecting a 'file' property\n", cfg.Type());
        return nullptr;
    }
    fname = cfg.ResolvePath(file);
    fs = ctx->platform->OpenFileSystem(fname);
    free(fname);
    return fs;
}


static Device *device_create(const DeviceConfig &cfg, DeviceContext *ctx)
{
    const char *type = cfg.Type();

    if (strcmp(type, "dwmac") == 0) {
        const char *compatible, *phy_mode;
        /* Which controller this claims to be decides which driver binds to
           it, so it is worth setting from the configuration rather than
           being fixed here. */
        if (!cfg.GetStrOpt("compatible", &compatible) ||
            !cfg.GetStrOpt("phy_mode", &phy_mode)) {
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
        JSONValue list = cfg.Get("quirks");
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
        auto net = node_open_ethernet(cfg, ctx);
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
        if (!cfg.GetInt("reg", &port, -1) ||
            !cfg.GetInt("irq", &irq, -1)) {
            return nullptr;
        }
        auto net = node_open_ethernet(cfg, ctx);
        if (net == nullptr) {
            return nullptr;
        }
        return new NE2000Device(ctx, std::move(net), cfg.IdOr("ne2000"),
                                port, irq);
    }

    if (strcmp(type, "ethernet-phy") == 0) {
        int address, phy_id;
        /* Without an address the bus places the PHY, exactly as the MMIO
           allocator places a device that names no base. */
        if (!cfg.GetInt("reg", &address, -1) ||
            !cfg.GetInt("phy_id", &phy_id, PHY_GENERIC_ID)) {
            return nullptr;
        }
        return new PHYDevice(address, (uint32_t)phy_id);
    }

    if (strcmp(type, "virtio-block") == 0) {
        int read_only;
        if (!cfg.GetInt("read_only", &read_only, 0)) {
            return nullptr;
        }
        auto bs = node_open_block(cfg, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        return virtio_block_node_create(ctx, std::move(bs), read_only != 0);
    }

    if (strcmp(type, "virtio-scsi") == 0) {
        return virtio_scsi_node_create(ctx);
    }

    if (strcmp(type, "virtio-net") == 0) {
        auto net = node_open_ethernet(cfg, ctx);
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
        if (!cfg.GetStr("tag", &tag)) {
            return nullptr;
        }
        auto fs = node_open_fs(cfg, ctx);
        if (fs == nullptr) {
            return nullptr;
        }
        return virtio_9p_node_create(ctx, std::move(fs), tag);
    }

    if (strcmp(type, "virtio-input") == 0) {
        const char *kind;
        VirtioInputTypeEnum input_type;
        if (!cfg.GetStr("kind", &kind)) {
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
        if (!cfg.GetInt("width", &width, 1024) ||
            !cfg.GetInt("height", &height, 768)) {
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

    vm_error("unsupported device type: %s\n", type);
    return nullptr;
}


/* The device types not yet defined next to their devices. */
class LegacyDeviceClass final: public DeviceClass {
public:
    using DeviceClass::DeviceClass;

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
        {return device_create(cfg, ctx);}
};

static const LegacyDeviceClass sLegacyClasses[] = {
    LegacyDeviceClass("dwmac"),
    LegacyDeviceClass("ne2000"),
    LegacyDeviceClass("ethernet-phy"),
    LegacyDeviceClass("virtio-block"),
    LegacyDeviceClass("virtio-scsi"),
    LegacyDeviceClass("virtio-net"),
    LegacyDeviceClass("virtio-console"),
    LegacyDeviceClass("virtio-9p"),
    LegacyDeviceClass("virtio-input"),
    LegacyDeviceClass("virtio-gpu"),
};


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
