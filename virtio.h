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

#include <sys/select.h>

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

#define VIRTIO_DEBUG_IO (1 << 0)
#define VIRTIO_DEBUG_9P (1 << 1)

void virtio_set_debug(VIRTIODevice *s, int debug_flags);

/* block device */

/* Notified when an asynchronous block request finishes. */
class BlockDeviceCompletion {
public:
    virtual ~BlockDeviceCompletion() = default;

    virtual void Complete(int ret) = 0;
};


/* A completion pointer may be null when the caller does not care; requests
   that finish synchronously return 0 without notifying. */
class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    virtual int64_t SectorCount() = 0;
    virtual int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                          BlockDeviceCompletion *completion) = 0;
    virtual int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                           BlockDeviceCompletion *completion) = 0;
};

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs);

/* network device */

/* The device half of the link, implemented by virtio-net and installed on
   the host-side EthernetDevice once the device exists. */
class EthernetTarget {
public:
    virtual ~EthernetTarget() = default;

    virtual bool CanWritePacket() = 0;
    virtual void WritePacket(const uint8_t *buf, int len) = 0;
    virtual void SetCarrier(bool carrier_state) = 0;
};


/* The host half of the link: a tun interface, slirp, ... */
class EthernetDevice {
public:
    uint8_t mac_addr[6] {}; /* mac address of the interface */
    EthernetTarget *target = nullptr; /* set by the device */

    virtual ~EthernetDevice() = default;

    virtual void WritePacket(const uint8_t *buf, int len) = 0;
    virtual void SelectFill(int *pfd_max, fd_set *rfds, fd_set *wfds,
                            fd_set *efds, int *pdelay) = 0;
    virtual void SelectPoll(fd_set *rfds, fd_set *wfds, fd_set *efds,
                            int select_ret) = 0;
};

VIRTIODevice *virtio_net_init(VIRTIOBusDef *bus, EthernetDevice *es);

/* console device */

class CharacterDevice {
public:
    virtual ~CharacterDevice() = default;

    virtual void WriteData(const uint8_t *buf, int len) = 0;
    virtual int ReadData(uint8_t *buf, int len) = 0;
};

VIRTIODevice *virtio_console_init(VIRTIOBusDef *bus, CharacterDevice *cs);
bool virtio_console_can_write_data(VIRTIODevice *s);
int virtio_console_get_write_len(VIRTIODevice *s);
int virtio_console_write_data(VIRTIODevice *s, const uint8_t *buf, int buf_len);
void virtio_console_resize_event(VIRTIODevice *s, int width, int height);

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

VIRTIODevice *virtio_input_init(VIRTIOBusDef *bus, VirtioInputTypeEnum type);

/* 9p filesystem device */

#include "fs.h"

VIRTIODevice *virtio_9p_init(VIRTIOBusDef *bus, FSDevice *fs,
                             const char *mount_tag);

#endif /* VIRTIO_H */
