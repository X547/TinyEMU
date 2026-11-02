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

#include "hid_report.h"
#include "host_time.h"
#include "machine.h"


//#pragma mark - HIDCollection

void HIDCollection::SendInputReport(const uint8_t *data, int len)
{
    if (fDevice != nullptr) {
        fDevice->QueueInputReport(this, data, len);
    }
}


int HIDCollection::GetReport(uint8_t type, uint8_t *buf, int size)
{
    if (type != HID_REPORT_INPUT) {
        return -1;
    }
    return BuildInputReport(buf, size);
}


//#pragma mark - HIDDevice

bool HIDDevice::AddCollection(HIDCollection *c)
{
    if (fCount >= HID_MAX_COLLECTIONS) {
        vm_error("%s: holds at most %d collections\n", fName,
                 HID_MAX_COLLECTIONS);
        return false;
    }
    fCollections[fCount++] = c;
    c->fDevice = this;

    HIDReportBuilder r;
    for (int i = 0; i < fCount; i++) {
        if (UsesReportIds()) {
            r.NextReportId(i + 1);
        }
        fCollections[i]->BuildReportDescriptor(r);
    }
    fReportDesc = r.Take();
    return fTransport->CollectionsChanged(this);
}


int HIDDevice::IndexOf(const HIDCollection *c) const
{
    for (int i = 0; i < fCount; i++) {
        if (fCollections[i] == c) {
            return i;
        }
    }
    return -1;
}


/* The collection with a boot format, when exactly one has one. */
int HIDDevice::BootCollection() const
{
    int found = -1;
    for (int i = 0; i < fCount; i++) {
        if (fCollections[i]->BootProtocol() == HID_BOOT_NONE) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = i;
    }
    return found;
}


/* The collection a request for a report ID reaches in the active protocol,
   or -1. */
int HIDDevice::CollectionForId(int id) const
{
    if (fBootProtocol) {
        return id == 0 ? BootCollection() : -1;
    }
    if (!UsesReportIds()) {
        return id == 0 && fCount > 0 ? 0 : -1;
    }
    return id >= 1 && id <= fCount ? id - 1 : -1;
}


HIDBootProtocolEnum HIDDevice::BootProtocol() const
{
    int i = BootCollection();
    return i < 0 ? HID_BOOT_NONE : fCollections[i]->BootProtocol();
}


uint8_t HIDDevice::CountryCode() const
{
    for (int i = 0; i < fCount; i++) {
        if (fCollections[i]->CountryCode() != 0) {
            return fCollections[i]->CountryCode();
        }
    }
    return 0;
}


/* Polled as often as the most demanding collection asks. */
int HIDDevice::PollInterval() const
{
    int ms = 0;
    for (int i = 0; i < fCount; i++) {
        int p = fCollections[i]->PollInterval();
        if (ms == 0 || p < ms) {
            ms = p;
        }
    }
    return ms != 0 ? ms : 10;
}


int HIDDevice::MaxInputReportSize() const
{
    int size = 0;
    for (int i = 0; i < fCount; i++) {
        int s = fCollections[i]->InputReportSize();
        size = s > size ? s : size;
    }
    return size + (UsesReportIds() ? 1 : 0);
}


int HIDDevice::MaxOutputReportSize() const
{
    int size = 0;
    for (int i = 0; i < fCount; i++) {
        int s = fCollections[i]->OutputReportSize();
        size = s > size ? s : size;
    }
    return size == 0 ? 0 : size + (UsesReportIds() ? 1 : 0);
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


void HIDDevice::QueueInputReport(HIDCollection *c, const uint8_t *data,
                                 int len)
{
    int index = IndexOf(c);
    if (index < 0 || len <= 0) {
        return;
    }
    if (fBootProtocol && index != BootCollection()) {
        /* It has no boot format, so the host is not listening for it. */
        return;
    }
    uint8_t buf[HID_MAX_REPORT_SIZE];
    int id_len = !fBootProtocol && UsesReportIds() ? 1 : 0;
    if (len > HID_MAX_REPORT_SIZE - id_len) {
        len = HID_MAX_REPORT_SIZE - id_len;
    }
    if (id_len != 0) {
        buf[0] = index + 1;
    }
    memcpy(buf + id_len, data, len);
    Enqueue(index, buf, id_len + len);
}


/* A report as it goes on the wire, for the transport to collect. */
void HIDDevice::Enqueue(int index, const uint8_t *data, int len)
{
    if (fQueueCount >= HID_REPORT_QUEUE_SIZE) {
        /* Nothing is collecting the reports; the oldest goes, which for a
           pointer leaves the current position. */
        fQueueHead = (fQueueHead + 1) % HID_REPORT_QUEUE_SIZE;
        fQueueCount--;
    }

    QueuedReport &report =
        fQueue[(fQueueHead + fQueueCount) % HID_REPORT_QUEUE_SIZE];
    memcpy(report.data, data, len);
    report.len = len;
    report.collection = index;
    fQueueCount++;

    fTransport->HandleInputReport(this);
}


/* A report of one collection went out, so its idle period starts over. */
void HIDDevice::ReportSent(int index)
{
    fIdleDue[index] = host_monotonic_us() + fIdle[index] * (uint64_t)1000;
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
    ReportSent(report.collection);
    return len;
}


/* The current report of one collection, as the active protocol sends it. */
int HIDDevice::BuildReport(int index, uint8_t type, uint8_t *buf, int size)
{
    int id_len = !fBootProtocol && UsesReportIds() ? 1 : 0;
    if (size < id_len) {
        return -1;
    }
    int len = fCollections[index]->GetReport(type, buf + id_len,
                                             size - id_len);
    if (len < 0) {
        return -1;
    }
    if (id_len != 0) {
        buf[0] = index + 1;
    }
    return id_len + len;
}


int HIDDevice::GetReport(uint8_t type, int id, uint8_t *buf, int size)
{
    int index = CollectionForId(id);
    if (index < 0) {
        return -1;
    }
    return BuildReport(index, type, buf, size);
}


bool HIDDevice::SetReport(uint8_t type, int id, const uint8_t *buf, int len)
{
    if (!fBootProtocol && UsesReportIds()) {
        /* The report starts with its ID, which has to be the one asked
           for. */
        if (len < 1 || (id >= 0 && buf[0] != id)) {
            return false;
        }
        id = buf[0];
        buf++;
        len--;
    } else if (id < 0) {
        id = 0;
    }
    int index = CollectionForId(id);
    return index >= 0 && fCollections[index]->SetReport(type, buf, len);
}


int HIDDevice::Idle(int id) const
{
    if (fCount == 0) {
        return -1;
    }
    if (id == 0) {
        return fIdle[0];
    }
    if (!UsesReportIds() || id > fCount) {
        return -1;
    }
    return fIdle[id - 1];
}


bool HIDDevice::SetIdle(int id, uint16_t ms)
{
    if (id != 0 && (!UsesReportIds() || id > fCount)) {
        return false;
    }
    for (int i = 0; i < fCount; i++) {
        if (id == 0 || i == id - 1) {
            /* The new period counts from now. */
            fIdle[i] = ms;
            ReportSent(i);
        }
    }
    return true;
}


uint64_t HIDDevice::NextIdleDue() const
{
    int boot = fBootProtocol ? BootCollection() : -1;
    uint64_t due = UINT64_MAX;
    for (int i = 0; i < fCount; i++) {
        if (fIdle[i] != 0 && (!fBootProtocol || i == boot) &&
            fIdleDue[i] < due) {
            due = fIdleDue[i];
        }
    }
    return due;
}


/* The collection whose idle period ran out first without a change, or
   -1. */
int HIDDevice::DueCollection() const
{
    uint64_t now = host_monotonic_us();
    int boot = fBootProtocol ? BootCollection() : -1;
    int index = -1;
    for (int i = 0; i < fCount; i++) {
        if (fIdle[i] != 0 && (!fBootProtocol || i == boot) &&
            fIdleDue[i] <= now &&
            (index < 0 || fIdleDue[i] < fIdleDue[index])) {
            index = i;
        }
    }
    return index;
}


/* Such a collection sends its current state again. */
int HIDDevice::TakeIdleReport(uint8_t *buf, int size)
{
    int index = DueCollection();
    if (index < 0) {
        return 0;
    }
    int len = BuildReport(index, HID_REPORT_INPUT, buf, size);
    if (len <= 0) {
        return 0;
    }
    ReportSent(index);
    return len;
}


bool HIDDevice::QueueIdleReport()
{
    uint8_t buf[HID_MAX_REPORT_SIZE];
    int index = DueCollection();
    if (index < 0) {
        return false;
    }
    int len = BuildReport(index, HID_REPORT_INPUT, buf, sizeof(buf));
    if (len <= 0) {
        return false;
    }
    /* The period counts from now, not from when the host reads it. */
    ReportSent(index);
    Enqueue(index, buf, len);
    return true;
}


void HIDDevice::Reset()
{
    fBootProtocol = false;
    fQueueHead = 0;
    fQueueCount = 0;
    for (int i = 0; i < fCount; i++) {
        fIdle[i] = 0;
        fCollections[i]->Reset();
    }
}


//#pragma mark - HIDBus

bool HIDBus::AssignResources(Device *dev)
{
    /* A collection reaches the host only through its transport, so it holds
       no address space and no interrupt line of its own. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("hid bus: device '%s' declared a resource, but a HID "
                     "collection has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - HIDCollectionNode

HIDCollectionNode::HIDCollectionNode(const char *name, HIDCollection *c):
    Device(name), fCollection(c)
{
}


HIDCollectionNode::~HIDCollectionNode()
{
    delete fCollection;
}


bool HIDCollectionNode::Realize()
{
    HIDBus *bus = dynamic_cast<HIDBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a HID bus\n", Name());
        return false;
    }
    return bus->Dev()->AddCollection(fCollection);
}
