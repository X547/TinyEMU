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
#pragma once

#include "virtio.h"


#define MAX_QUEUE_NUM 32

#define MAX_QUEUE 8
#define MAX_CONFIG_SPACE_SIZE 256

typedef struct {
    uint32_t ready; /* 0 or 1 */
    uint32_t num;
    uint16_t last_avail_idx;
    virtio_phys_addr_t desc_addr;
    virtio_phys_addr_t avail_addr;
    virtio_phys_addr_t used_addr;
    bool manual_recv; /* if true, the device_recv() callback is not called */
} QueueState;

class VIRTIOTransport;

struct VIRTIODevice: public PCIBarTarget {
    PhysMemoryMap *mem_map = nullptr;
    PhysMemoryRange *mem_range = nullptr;
    /* PCI only */
    PCIDevice *pci_dev = nullptr;
    /* MMIO only */
    IRQSignal *irq = nullptr;
    /* owns the register layout and the DMA path for this bus */
    VIRTIOTransport *transport = nullptr;
    int debug = 0;

    uint32_t int_status = 0;
    uint32_t status = 0;
    uint32_t device_features_sel = 0;
    uint32_t queue_sel = 0; /* currently selected queue */
    QueueState queue[MAX_QUEUE] {};

    /* device specific */
    uint32_t device_id = 0;
    uint32_t vendor_id = 0;
    uint32_t device_features = 0;
    uint32_t config_space_size = 0; /* in bytes, must be multiple of 4 */
    uint8_t config_space[MAX_CONFIG_SPACE_SIZE] {};

    virtual ~VIRTIODevice() = default;

    /* return < 0 to stop the notification (it must be manually restarted
       later), 0 if OK */
    virtual int RecvRequest(int queue_idx, int desc_idx, int read_size,
                            int write_size) = 0;
    /* called after the config is written */
    virtual void ConfigWrite() {}

    /* return nullptr if no RAM at this address. The mapping is valid for
       one page */
    uint8_t *GetRamPtr(virtio_phys_addr_t paddr, bool is_rw);

    void SetBar(int bar_num, uint32_t addr, bool enabled) override;
};


/* Implemented in virtio.cpp and shared with the per-device sources. */
void virtio_init(VIRTIODevice *s, VIRTIOBusDef *bus, uint32_t device_id,
                 int config_space_size);

uint16_t virtio_read16(VIRTIODevice *s, virtio_phys_addr_t addr);
int memcpy_to_queue(VIRTIODevice *s, int queue_idx, int desc_idx, int offset,
                    const void *buf, int count);
int memcpy_from_queue(VIRTIODevice *s, void *buf, int queue_idx, int desc_idx,
                      int offset, int count);
int get_desc_rw_size(VIRTIODevice *s, int *pread_size, int *pwrite_size,
                     int queue_idx, int desc_idx);
void virtio_consume_desc(VIRTIODevice *s, int queue_idx, int desc_idx,
                         int desc_len);
void queue_notify(VIRTIODevice *s, int queue_idx);
void virtio_config_change_notify(VIRTIODevice *s);
