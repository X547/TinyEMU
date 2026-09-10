/*
 * HID device model and report transport
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
#include "hid.h"

#include <string.h>

#include "machine.h"


//#pragma mark - HIDDevice

void HIDDevice::Reset()
{
    fBootProtocol = false;
    fQueueHead = 0;
    fQueueCount = 0;
}


void HIDDevice::SetProtocol(bool boot_protocol)
{
    if (boot_protocol == fBootProtocol) {
        return;
    }
    fBootProtocol = boot_protocol;
    /* Queued reports are in the other format. */
    fQueueHead = 0;
    fQueueCount = 0;
}


void HIDDevice::QueueInputReport(const uint8_t *data, int len)
{
    if (len <= 0) {
        return;
    }
    if (len > HID_MAX_REPORT_SIZE) {
        len = HID_MAX_REPORT_SIZE;
    }

    if (fQueueCount >= HID_REPORT_QUEUE_SIZE) {
        /* Nothing is collecting the reports; the oldest goes. */
        fQueueHead = (fQueueHead + 1) % HID_REPORT_QUEUE_SIZE;
        fQueueCount--;
    }

    int slot = (fQueueHead + fQueueCount) % HID_REPORT_QUEUE_SIZE;
    memcpy(fQueue[slot].data, data, len);
    fQueue[slot].len = len;
    fQueueCount++;

    if (fTransport != nullptr) {
        fTransport->HandleInputReport(this);
    }
}


int HIDDevice::TakeInputReport(uint8_t *buf, int size)
{
    if (fQueueCount == 0) {
        return 0;
    }
    QueuedReport &report = fQueue[fQueueHead];
    int len = report.len < size ? report.len : size;

    memcpy(buf, report.data, len);
    fQueueHead = (fQueueHead + 1) % HID_REPORT_QUEUE_SIZE;
    fQueueCount--;
    return len;
}


int HIDDevice::GetReport(uint8_t type, uint8_t *buf, int size)
{
    if (type != HID_REPORT_INPUT) {
        return -1;
    }
    return BuildInputReport(buf, size);
}


//#pragma mark - HIDBus

bool HIDBus::AssignResources(Device *dev)
{
    /* A HID function reaches the host only through its transport, so it holds
       no address space and no interrupt line of its own. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("hid bus: device '%s' declared a resource, but a HID "
                     "function has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - HIDDeviceNode

HIDDeviceNode::HIDDeviceNode(const char *name, HIDDevice *dev, int index):
    Device(name), fDev(dev), fIndex(index)
{
}


HIDDeviceNode::~HIDDeviceNode()
{
    delete fDev;
}


bool HIDDeviceNode::Realize()
{
    HIDBus *bus = dynamic_cast<HIDBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a HID bus\n", Name());
        return false;
    }

    HIDBusTarget *target = bus->Target();
    int index = fIndex;
    if (index < 0) {
        index = target->FindFreeIndex();
        if (index < 0) {
            vm_error("%s: the transport has no free function left\n", Name());
            return false;
        }
    }
    return target->AttachDevice(fDev, index);
}
