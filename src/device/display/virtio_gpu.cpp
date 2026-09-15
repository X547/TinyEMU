/*
 * VIRTIO GPU device, 2D
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <new>
#include <vector>

#include "cutils.h"
#include "virtio.h"
#include "virtio_priv.h"

//#define DEBUG_VIRTIO_GPU

#define VIRTIO_GPU_DEVICE_ID 16

/* control queue */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107

/* cursor queue */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

#define VIRTIO_GPU_FLAG_FENCE    1
#define VIRTIO_GPU_EVENT_DISPLAY 1

#define VIRTIO_GPU_MAX_SCANOUTS 16

/* Byte order of the pixels in memory. */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM 121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM 134

/* configuration space */
#define VIRTIO_GPU_CONFIG_EVENTS_READ  0
#define VIRTIO_GPU_CONFIG_EVENTS_CLEAR 4
#define VIRTIO_GPU_CONFIG_NUM_SCANOUTS 8
#define VIRTIO_GPU_CONFIG_NUM_CAPSETS  12
#define VIRTIO_GPU_CONFIG_SIZE         16

#define VIRTIO_CONFIG_S_DRIVER_OK 4

/* struct virtio_gpu_ctrl_hdr */
#define CTRL_HDR_SIZE 24
/* the longest command without its trailing array */
#define CMD_MAX_SIZE 64
#define DISPLAY_INFO_SIZE (CTRL_HDR_SIZE + VIRTIO_GPU_MAX_SCANOUTS * 24)
#define MEM_ENTRY_SIZE 16

/* Limits on what a guest may make the host allocate. */
#define MAX_RESOURCE_SIZE 16384
#define MAX_HOST_MEMORY ((uint64_t)256 << 20)
#define MAX_BACKING_ENTRIES 65536
#define MAX_CURSOR_SIZE 256


struct GPURect {
    uint32_t x, y, width, height;
};


static GPURect get_rect(const uint8_t *p)
{
    GPURect r;
    r.x = get_le32(p);
    r.y = get_le32(p + 4);
    r.width = get_le32(p + 8);
    r.height = get_le32(p + 12);
    return r;
}


struct GPUMemEntry {
    uint64_t addr;
    uint32_t length;
};


/* A 2D resource. Its pixels are kept in the host's xRGB order whatever the
   format, so that the screen and the cursor take them as they are. */
struct GPUResource {
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::unique_ptr<uint8_t[]> data;
    /* guest pages; 'backing_start' is the offset each entry begins at */
    std::vector<GPUMemEntry> backing;
    std::vector<uint64_t> backing_start;
    uint64_t backing_size = 0;

    uint32_t Stride() const {return width * 4;}
    uint64_t Size() const {return (uint64_t)width * height * 4;}

    bool Contains(const GPURect &r) const
    {
        return (uint64_t)r.x + r.width <= width &&
            (uint64_t)r.y + r.height <= height;
    }
};


static bool format_supported(uint32_t format)
{
    switch (format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        return true;
    default:
        return false;
    }
}


static bool format_has_alpha(uint32_t format)
{
    switch (format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        return true;
    default:
        return false;
    }
}


/* Reorders 'count' pixels in place into B, G, R, A bytes. */
static void convert_pixels(uint8_t *p, uint32_t count, uint32_t format)
{
    uint8_t c0, c1, c2, c3;

    switch (format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        break;
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        for (; count > 0; count--, p += 4) {
            c0 = p[0]; c1 = p[1]; c2 = p[2]; c3 = p[3];
            p[0] = c3; p[1] = c2; p[2] = c1; p[3] = c0;
        }
        break;
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        for (; count > 0; count--, p += 4) {
            c0 = p[0];
            p[0] = p[2];
            p[2] = c0;
        }
        break;
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        for (; count > 0; count--, p += 4) {
            c0 = p[0];
            p[0] = p[1]; p[1] = p[2]; p[2] = p[3];
            p[3] = c0;
        }
        break;
    }
}


struct VIRTIOGPUDevice final: public VIRTIODevice, public ScreenSource {
    HostScreen *screen = nullptr;
    /* the display size offered to the guest */
    int display_width = 0;
    int display_height = 0;
    uint32_t events_read = 0;

    std::map<uint32_t, std::unique_ptr<GPUResource>> resources;
    uint64_t host_memory = 0;
    /* scanout 0; resource 0 when disabled */
    uint32_t scanout_resource = 0;
    GPURect scanout_rect {};

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void ConfigWrite() override;
    void Reset() override;

    /* ScreenSource */
    void SetScreen(HostScreen *screen) override;
    bool Resizable() override {return true;}
    void ScreenResized(int width, int height) override;

    void UpdateConfig();
    GPUResource *FindResource(uint32_t id);
    void DisableScanout();
    bool CopyFromRam(uint64_t addr, uint8_t *dst, uint64_t len);
    bool CopyFromBacking(const GPUResource *res, uint64_t offset,
                         uint8_t *dst, uint64_t len);

    uint32_t GetDisplayInfo(uint8_t *resp, int *resp_len);
    uint32_t ResourceCreate2D(const uint8_t *cmd);
    uint32_t ResourceUnref(const uint8_t *cmd);
    uint32_t SetScanout(const uint8_t *cmd);
    uint32_t ResourceFlush(const uint8_t *cmd);
    uint32_t TransferToHost2D(const uint8_t *cmd);
    uint32_t AttachBacking(const uint8_t *cmd, int queue_idx, int desc_idx,
                           int read_size);
    uint32_t DetachBacking(const uint8_t *cmd);
    uint32_t UpdateCursor(const uint8_t *cmd, bool move_only);
};


void VIRTIOGPUDevice::UpdateConfig()
{
    put_le32(config_space + VIRTIO_GPU_CONFIG_EVENTS_READ, events_read);
    put_le32(config_space + VIRTIO_GPU_CONFIG_EVENTS_CLEAR, 0);
    put_le32(config_space + VIRTIO_GPU_CONFIG_NUM_SCANOUTS, 1);
    put_le32(config_space + VIRTIO_GPU_CONFIG_NUM_CAPSETS, 0);
}


void VIRTIOGPUDevice::ConfigWrite()
{
    events_read &= ~get_le32(config_space + VIRTIO_GPU_CONFIG_EVENTS_CLEAR);
    UpdateConfig();
}


void VIRTIOGPUDevice::Reset()
{
    DisableScanout();
    if (screen != nullptr)
        screen->SetCursor(nullptr, 0, 0);
    resources.clear();
    host_memory = 0;
    events_read = 0;
    UpdateConfig();
}


void VIRTIOGPUDevice::SetScreen(HostScreen *new_screen)
{
    screen = new_screen;
}


void VIRTIOGPUDevice::ScreenResized(int width, int height)
{
    width = std::clamp(width, VIRTIO_GPU_MIN_SIZE, VIRTIO_GPU_MAX_SIZE);
    height = std::clamp(height, VIRTIO_GPU_MIN_SIZE, VIRTIO_GPU_MAX_SIZE);
    if (width == display_width && height == display_height)
        return;
    display_width = width;
    display_height = height;
    /* the driver asks for the new size when it sees the event */
    events_read |= VIRTIO_GPU_EVENT_DISPLAY;
    UpdateConfig();
    if (status & VIRTIO_CONFIG_S_DRIVER_OK)
        virtio_config_change_notify(this);
}


GPUResource *VIRTIOGPUDevice::FindResource(uint32_t id)
{
    auto it = resources.find(id);
    return it != resources.end() ? it->second.get() : nullptr;
}


void VIRTIOGPUDevice::DisableScanout()
{
    if (scanout_resource == 0)
        return;
    scanout_resource = 0;
    if (screen != nullptr) {
        screen->SetFramebuffer(nullptr, scanout_rect.width,
                               scanout_rect.height, 0);
    }
}


bool VIRTIOGPUDevice::CopyFromRam(uint64_t addr, uint8_t *dst, uint64_t len)
{
    while (len > 0) {
        uint64_t l = std::min<uint64_t>(len, VIRTIO_PAGE_SIZE -
                                        (addr & (VIRTIO_PAGE_SIZE - 1)));
        uint8_t *ptr = GetRamPtr(addr, false);
        if (ptr == nullptr)
            return false;
        memcpy(dst, ptr, l);
        addr += l;
        dst += l;
        len -= l;
    }
    return true;
}


bool VIRTIOGPUDevice::CopyFromBacking(const GPUResource *res, uint64_t offset,
                                      uint8_t *dst, uint64_t len)
{
    if (offset > res->backing_size || len > res->backing_size - offset)
        return false;
    if (len == 0)
        return true;
    /* the last entry that starts at or before 'offset' */
    size_t i = std::upper_bound(res->backing_start.begin(),
                                res->backing_start.end(), offset) -
        res->backing_start.begin() - 1;
    offset -= res->backing_start[i];
    while (len > 0) {
        const GPUMemEntry &e = res->backing[i];
        uint64_t l = std::min<uint64_t>(len, e.length - offset);
        if (!CopyFromRam(e.addr + offset, dst, l))
            return false;
        dst += l;
        len -= l;
        offset = 0;
        i++;
    }
    return true;
}


uint32_t VIRTIOGPUDevice::GetDisplayInfo(uint8_t *resp, int *resp_len)
{
    uint8_t *pmode = resp + CTRL_HDR_SIZE;

    memset(pmode, 0, DISPLAY_INFO_SIZE - CTRL_HDR_SIZE);
    put_le32(pmode + 8, display_width);
    put_le32(pmode + 12, display_height);
    put_le32(pmode + 16, 1); /* enabled */
    *resp_len = DISPLAY_INFO_SIZE;
    return VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
}


uint32_t VIRTIOGPUDevice::ResourceCreate2D(const uint8_t *cmd)
{
    uint32_t id = get_le32(cmd + 24);
    auto res = std::make_unique<GPUResource>();

    res->format = get_le32(cmd + 28);
    res->width = get_le32(cmd + 32);
    res->height = get_le32(cmd + 36);

    if (id == 0 || FindResource(id) != nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!format_supported(res->format) ||
        res->width == 0 || res->width > MAX_RESOURCE_SIZE ||
        res->height == 0 || res->height > MAX_RESOURCE_SIZE)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    if (res->Size() > MAX_HOST_MEMORY - host_memory)
        return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
    res->data.reset(new (std::nothrow) uint8_t[res->Size()]());
    if (res->data == nullptr)
        return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;

    host_memory += res->Size();
    resources[id] = std::move(res);
    return VIRTIO_GPU_RESP_OK_NODATA;
}


uint32_t VIRTIOGPUDevice::ResourceUnref(const uint8_t *cmd)
{
    uint32_t id = get_le32(cmd + 24);
    GPUResource *res = FindResource(id);

    if (res == nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (scanout_resource == id)
        DisableScanout();
    host_memory -= res->Size();
    resources.erase(id);
    return VIRTIO_GPU_RESP_OK_NODATA;
}


uint32_t VIRTIOGPUDevice::SetScanout(const uint8_t *cmd)
{
    GPURect r = get_rect(cmd + 24);
    uint32_t scanout_id = get_le32(cmd + 40);
    uint32_t id = get_le32(cmd + 44);

    if (scanout_id != 0)
        return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
    if (id == 0) {
        DisableScanout();
        return VIRTIO_GPU_RESP_OK_NODATA;
    }
    GPUResource *res = FindResource(id);
    if (res == nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (r.width == 0 || r.height == 0 || !res->Contains(r))
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    scanout_resource = id;
    scanout_rect = r;
    if (screen != nullptr) {
        screen->SetFramebuffer(res->data.get() + r.y * res->Stride() + r.x * 4,
                               r.width, r.height, res->Stride());
    }
    return VIRTIO_GPU_RESP_OK_NODATA;
}


uint32_t VIRTIOGPUDevice::ResourceFlush(const uint8_t *cmd)
{
    GPURect r = get_rect(cmd + 24);
    uint32_t id = get_le32(cmd + 40);

    if (FindResource(id) == nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (id != scanout_resource || screen == nullptr)
        return VIRTIO_GPU_RESP_OK_NODATA;

    /* the part of the rectangle the scanout shows, in scanout pixels */
    const GPURect &s = scanout_rect;
    int64_t x0 = std::max<int64_t>(r.x, s.x);
    int64_t y0 = std::max<int64_t>(r.y, s.y);
    int64_t x1 = std::min<int64_t>((int64_t)r.x + r.width,
                                   (int64_t)s.x + s.width);
    int64_t y1 = std::min<int64_t>((int64_t)r.y + r.height,
                                   (int64_t)s.y + s.height);
    if (x0 < x1 && y0 < y1)
        screen->Update(x0 - s.x, y0 - s.y, x1 - x0, y1 - y0);
    return VIRTIO_GPU_RESP_OK_NODATA;
}


uint32_t VIRTIOGPUDevice::TransferToHost2D(const uint8_t *cmd)
{
    GPURect r = get_rect(cmd + 24);
    uint64_t offset = get_le64(cmd + 40);
    uint32_t id = get_le32(cmd + 48);
    GPUResource *res = FindResource(id);

    if (res == nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!res->Contains(r))
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    if (res->backing.empty())
        return VIRTIO_GPU_RESP_ERR_UNSPEC;

    uint32_t stride = res->Stride();
    if (r.x == 0 && r.y == 0 && r.width == res->width &&
        r.height == res->height) {
        /* the whole resource, from the start of the backing */
        if (!CopyFromBacking(res, 0, res->data.get(), res->Size()))
            return VIRTIO_GPU_RESP_ERR_UNSPEC;
        convert_pixels(res->data.get(), res->width * res->height, res->format);
        return VIRTIO_GPU_RESP_OK_NODATA;
    }
    /* 'offset' is where row r.y, column r.x is in the backing */
    for (uint32_t h = 0; h < r.height; h++) {
        uint8_t *dst = res->data.get() + (uint64_t)(r.y + h) * stride + r.x * 4;
        if (!CopyFromBacking(res, offset + (uint64_t)stride * h, dst,
                             (uint64_t)r.width * 4))
            return VIRTIO_GPU_RESP_ERR_UNSPEC;
        convert_pixels(dst, r.width, res->format);
    }
    return VIRTIO_GPU_RESP_OK_NODATA;
}


uint32_t VIRTIOGPUDevice::AttachBacking(const uint8_t *cmd, int queue_idx,
                                        int desc_idx, int read_size)
{
    uint32_t id = get_le32(cmd + 24);
    uint32_t count = get_le32(cmd + 28);
    GPUResource *res = FindResource(id);

    if (res == nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!res->backing.empty())
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    if (count == 0 || count > MAX_BACKING_ENTRIES ||
        (uint64_t)read_size < 32 + (uint64_t)count * MEM_ENTRY_SIZE)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    std::vector<uint8_t> entries(count * MEM_ENTRY_SIZE);
    if (memcpy_from_queue(this, entries.data(), queue_idx, desc_idx, 32,
                          entries.size()) < 0)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    uint64_t size = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = entries.data() + i * MEM_ENTRY_SIZE;
        GPUMemEntry e;
        e.addr = get_le64(p);
        e.length = get_le32(p + 8);
        if (e.length == 0)
            continue;
        res->backing.push_back(e);
        res->backing_start.push_back(size);
        size += e.length;
    }
    res->backing_size = size;
    return VIRTIO_GPU_RESP_OK_NODATA;
}


uint32_t VIRTIOGPUDevice::DetachBacking(const uint8_t *cmd)
{
    GPUResource *res = FindResource(get_le32(cmd + 24));

    if (res == nullptr)
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    res->backing.clear();
    res->backing_start.clear();
    res->backing_size = 0;
    return VIRTIO_GPU_RESP_OK_NODATA;
}


/* The position is the top left corner of the image. */
uint32_t VIRTIOGPUDevice::UpdateCursor(const uint8_t *cmd, bool move_only)
{
    uint32_t scanout_id = get_le32(cmd + 24);
    int32_t x = get_le32(cmd + 28);
    int32_t y = get_le32(cmd + 32);

    if (scanout_id != 0)
        return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
    if (screen == nullptr)
        return VIRTIO_GPU_RESP_OK_NODATA;
    if (!move_only) {
        uint32_t id = get_le32(cmd + 40);
        if (id == 0) {
            screen->SetCursor(nullptr, 0, 0);
        } else {
            GPUResource *res = FindResource(id);
            if (res == nullptr)
                return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            if (res->width > MAX_CURSOR_SIZE || res->height > MAX_CURSOR_SIZE)
                return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            std::vector<uint32_t> pixels(res->width * res->height);
            uint32_t opaque = format_has_alpha(res->format) ? 0 : 0xff000000;
            for (size_t i = 0; i < pixels.size(); i++)
                pixels[i] = get_le32(res->data.get() + i * 4) | opaque;
            screen->SetCursor(pixels.data(), res->width, res->height);
        }
    }
    screen->MoveCursor(x, y);
    return VIRTIO_GPU_RESP_OK_NODATA;
}


int VIRTIOGPUDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                 int write_size)
{
    uint8_t cmd[CMD_MAX_SIZE] {};
    uint8_t resp[DISPLAY_INFO_SIZE];
    int len = min_int(read_size, CMD_MAX_SIZE);
    int resp_len = CTRL_HDR_SIZE;
    uint32_t type, resp_type;

    if (len < CTRL_HDR_SIZE ||
        memcpy_from_queue(this, cmd, queue_idx, desc_idx, 0, len) < 0) {
        virtio_consume_desc(this, queue_idx, desc_idx, 0);
        return 0;
    }
    type = get_le32(cmd);

#ifdef DEBUG_VIRTIO_GPU
    printf("virtio_gpu: queue=%d cmd=0x%04x read=%d write=%d\n", queue_idx,
           type, read_size, write_size);
#endif

    /* the command's size, so a short one reads no further than it goes */
    int need;
    switch (type) {
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:        need = CTRL_HDR_SIZE; break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:      need = 40; break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:          need = 32; break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:             need = 48; break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:          need = 48; break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:     need = 56; break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: need = 32; break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: need = 32; break;
    case VIRTIO_GPU_CMD_UPDATE_CURSOR:           need = 56; break;
    case VIRTIO_GPU_CMD_MOVE_CURSOR:             need = 56; break;
    default:                                     need = CTRL_HDR_SIZE; break;
    }

    if (len < need) {
        resp_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    } else if (queue_idx == 0) {
        switch (type) {
        case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
            resp_type = GetDisplayInfo(resp, &resp_len);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
            resp_type = ResourceCreate2D(cmd);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_UNREF:
            resp_type = ResourceUnref(cmd);
            break;
        case VIRTIO_GPU_CMD_SET_SCANOUT:
            resp_type = SetScanout(cmd);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
            resp_type = ResourceFlush(cmd);
            break;
        case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
            resp_type = TransferToHost2D(cmd);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
            resp_type = AttachBacking(cmd, queue_idx, desc_idx, read_size);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
            resp_type = DetachBacking(cmd);
            break;
        default:
            resp_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
    } else {
        switch (type) {
        case VIRTIO_GPU_CMD_UPDATE_CURSOR:
            resp_type = UpdateCursor(cmd, false);
            break;
        case VIRTIO_GPU_CMD_MOVE_CURSOR:
            resp_type = UpdateCursor(cmd, true);
            break;
        default:
            resp_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
    }

#ifdef DEBUG_VIRTIO_GPU
    if (resp_type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
        printf("virtio_gpu: cmd=0x%04x failed: 0x%04x\n", type, resp_type);
#endif

    memset(resp, 0, CTRL_HDR_SIZE);
    put_le32(resp, resp_type);
    if (get_le32(cmd + 4) & VIRTIO_GPU_FLAG_FENCE) {
        put_le32(resp + 4, VIRTIO_GPU_FLAG_FENCE);
        put_le64(resp + 8, get_le64(cmd + 8));
    }
    resp_len = min_int(resp_len, write_size);
    if (resp_len > 0)
        memcpy_to_queue(this, queue_idx, desc_idx, 0, resp, resp_len);
    virtio_consume_desc(this, queue_idx, desc_idx, resp_len);
    return 0;
}


std::unique_ptr<VIRTIODevice> virtio_gpu_init(VIRTIOBusDef *bus, int width,
                                              int height)
{
    auto s = std::make_unique<VIRTIOGPUDevice>();
    virtio_init(s.get(), bus, VIRTIO_GPU_DEVICE_ID, VIRTIO_GPU_CONFIG_SIZE);
    s->device_features = 0;
    s->display_width = width;
    s->display_height = height;
    s->UpdateConfig();
    return s;
}


ScreenSource *virtio_gpu_screen_source(VIRTIODevice *s)
{
    return static_cast<VIRTIOGPUDevice *>(s);
}
