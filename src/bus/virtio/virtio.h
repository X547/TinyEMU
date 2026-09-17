/*
 * VIRTIO driver
 * 
 * Copyright (c) 2016 Fabrice Bellard
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
#ifndef VIRTIO_H
#define VIRTIO_H

#include "host_block.h"
#include "host_console.h"
#include "host_ethernet.h"
#include "host_fs.h"
#include "host_screen.h"
#include "iomem.h"
#include "pci.h"

#define VIRTIO_PAGE_SIZE 4096

#define VIRTIO_ADDR_BITS 64

#if VIRTIO_ADDR_BITS == 64
typedef uint64_t virtio_phys_addr_t;
#else
typedef uint32_t virtio_phys_addr_t;
#endif

typedef struct {
    /* PCI only: */
    PCIBus *pci_bus;
    /* MMIO only: */
    PhysMemoryMap *mem_map;
    uint64_t addr;
    IRQSignal *irq;
} VIRTIOBusDef;

typedef struct VIRTIODevice VIRTIODevice; 

#define VIRTIO_DEBUG_IO bit_at(0)
#define VIRTIO_DEBUG_9P bit_at(1)

void virtio_set_debug(VIRTIODevice *s, int debug_flags);

/* block device */

std::unique_ptr<VIRTIODevice> virtio_block_init(VIRTIOBusDef *bus,
                                                HostBlockDevice *bs);

/* network device */

std::unique_ptr<VIRTIODevice> virtio_net_init(VIRTIOBusDef *bus,
                                              HostEthernet *es);

/* console device */

/* 'cs' may be null, which discards the output. */
std::unique_ptr<VIRTIODevice> virtio_console_init(VIRTIOBusDef *bus,
                                                  HostConsole *cs);
/* Where host input for a console device goes. */
ConsoleTarget *virtio_console_target(VIRTIODevice *s);

/* input device */

typedef enum {
    VIRTIO_INPUT_TYPE_KEYBOARD,
    VIRTIO_INPUT_TYPE_MOUSE,
    VIRTIO_INPUT_TYPE_TABLET,
} VirtioInputTypeEnum;

#define VIRTIO_INPUT_ABS_SCALE 32768

int virtio_input_send_key_event(VIRTIODevice *s, bool is_down,
                                uint16_t key_code);
int virtio_input_send_mouse_event(VIRTIODevice *s, int dx, int dy, int dz,
                                  unsigned int buttons);

std::unique_ptr<VIRTIODevice> virtio_input_init(VIRTIOBusDef *bus,
                                                VirtioInputTypeEnum type);

/* 9p filesystem device */

std::unique_ptr<VIRTIODevice> virtio_9p_init(VIRTIOBusDef *bus, HostFileSystem *fs,
                                             const char *mount_tag);

/* GPU device, 2D only */

/* the range of display sizes offered to the guest */
#define VIRTIO_GPU_MIN_SIZE 64
#define VIRTIO_GPU_MAX_SIZE 8192

/* 'width' x 'height' is the display size first offered to the guest. */
std::unique_ptr<VIRTIODevice> virtio_gpu_init(VIRTIOBusDef *bus, int width,
                                              int height);
ScreenSource *virtio_gpu_screen_source(VIRTIODevice *s);

/* device tree nodes */

class Device;
struct DeviceContext;

/* One wrapper serves every virtio device on either transport: which resources
   it takes is decided by the bus it is attached to, so the same node works on
   an MMIO machine and behind a PCI bridge. */
Device *virtio_block_node_create(DeviceContext *ctx,
                                 std::unique_ptr<HostBlockDevice> bs);
Device *virtio_net_node_create(DeviceContext *ctx,
                               std::unique_ptr<HostEthernet> net);
Device *virtio_console_node_create(DeviceContext *ctx);
Device *virtio_9p_node_create(DeviceContext *ctx,
                              std::unique_ptr<HostFileSystem> fs,
                              const char *mount_tag);
Device *virtio_input_node_create(DeviceContext *ctx, VirtioInputTypeEnum type);
Device *virtio_gpu_node_create(DeviceContext *ctx, int width, int height);

#endif /* VIRTIO_H */
