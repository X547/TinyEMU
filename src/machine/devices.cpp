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

#include "config_props.h"
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
        auto net = config_open_ethernet(cfg, ctx);
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
