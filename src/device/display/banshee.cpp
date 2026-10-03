/*
 * 3dfx Voodoo Banshee display adapter
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
#include "banshee.h"

#include <stdio.h>
#include <string.h>

#include "bits.h"
#include "cutils.h"
#include "simplefb.h"

//#define DEBUG_BANSHEE

#ifdef DEBUG_BANSHEE
#define banshee_trace(...) printf(__VA_ARGS__)
#else
#define banshee_trace(...) do {} while (0)
#endif


//#pragma mark - pixel formats

/* How many bits one pixel of a format occupies in memory. A 15 bit pixel is
   stored in a halfword like a 16 bit one. */
static int format_bpp(int format)
{
    switch (format) {
    case BLT_PIXEL_MONO:  return 1;
    case BLT_PIXEL_8BPP:  return 8;
    case BLT_PIXEL_15BPP: return 16;
    case BLT_PIXEL_16BPP: return 16;
    case BLT_PIXEL_24BPP: return 24;
    case BLT_PIXEL_32BPP: return 32;
    default:              return 0;
    }
}


/* A channel widened to eight bits by repeating its most significant bits,
   which is the conversion the blt engine performs. */
static uint32_t widen(uint32_t val, int bits)
{
    return (val << (8 - bits)) | (val >> (2 * bits - 8));
}


static uint32_t pixel_to_rgb(uint32_t val, int format,
                             const uint32_t *palette)
{
    switch (format) {
    case BLT_PIXEL_8BPP:
        return palette[val & 0xff];
    case BLT_PIXEL_15BPP:
        return (widen(get_bits(val, 10, 5), 5) << 16) |
            (widen(get_bits(val, 5, 5), 5) << 8) |
            widen(get_bits(val, 0, 5), 5);
    case BLT_PIXEL_16BPP:
        return (widen(get_bits(val, 11, 5), 5) << 16) |
            (widen(get_bits(val, 5, 6), 6) << 8) |
            widen(get_bits(val, 0, 5), 5);
    default:
        return val & 0xffffff;
    }
}


static uint32_t rgb_to_pixel(uint32_t rgb, int format)
{
    switch (format) {
    case BLT_PIXEL_15BPP:
        return (get_bits(rgb, 19, 5) << 10) | (get_bits(rgb, 11, 5) << 5) |
            get_bits(rgb, 3, 5);
    case BLT_PIXEL_16BPP:
        return (get_bits(rgb, 19, 5) << 11) | (get_bits(rgb, 10, 6) << 5) |
            get_bits(rgb, 3, 5);
    default:
        return rgb & 0xffffff;
    }
}


/* One ternary raster operation, applied to every bit of a pixel at once. The
   operation is indexed by the pattern, source and destination bits, which is
   what makes 0xcc a source copy and 0xf0 a pattern copy. */
static uint32_t apply_rop(uint8_t rop, uint32_t pat, uint32_t src,
                          uint32_t dst)
{
    uint32_t result = 0;

    for (int i = 0; i < 8; i++) {
        if (!get_bit(rop, i)) {
            continue;
        }
        uint32_t mask = get_bit(i, 2) ? pat : ~pat;
        mask &= get_bit(i, 1) ? src : ~src;
        mask &= get_bit(i, 0) ? dst : ~dst;
        result |= mask;
    }
    return result;
}


/* Whether a pixel falls in a colorkey range. Each channel is tested on its
   own above eight bits per pixel, which is what makes a range rather than an
   interval of the packed value. */
static bool colorkey_passes(uint32_t val, uint32_t min, uint32_t max, int bpp)
{
    if (bpp <= 8) {
        return val >= min && val <= max;
    }
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t c = get_bits(val, shift, 8);
        if (c < get_bits(min, shift, 8) || c > get_bits(max, shift, 8)) {
            return false;
        }
    }
    return true;
}


//#pragma mark - construction

BansheeDevice::BansheeDevice(DeviceContext *ctx, const char *name,
                             uint16_t device_id, int vram_mb, int init_width,
                             int init_height):
    Device(name),
    fCtx(ctx),
    fDeviceId(device_id),
    fVramSize((uint32_t)vram_mb << 20),
    fInitWidth(init_width),
    fInitHeight(init_height)
{
}


BansheeDevice::~BansheeDevice() = default;


bool BansheeDevice::Prepare()
{
    if (ParentBus() == nullptr || ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    return true;
}


bool BansheeDevice::Realize()
{
    PCIBus *pci_bus = ParentBus()->AsPCIBus();

    fPciDev = pci_register_device(pci_bus, Name(), -1, BANSHEE_PCI_VENDOR_ID,
                                  fDeviceId, 0x01, 0x0300);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI function\n", Name());
        return false;
    }
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    PhysMemoryMap *mem_map = pci_device_get_mem_map(fPciDev);
    PhysMemoryMap *port_map = pci_device_get_port_map(fPciDev);

    /* Video memory is plain RAM so that a guest drawing into the linear
       frame buffer costs nothing; the dirty bits are what the refresh walks
       to find what changed. */
    fLfbRange = mem_map->RegisterRam(0, fVramSize,
                                     DEVRAM_FLAG_DIRTY_BITS |
                                     DEVRAM_FLAG_DISABLED);
    if (fLfbRange == nullptr) {
        vm_error("%s: could not allocate %u MB of video memory\n", Name(),
                 fVramSize >> 20);
        return false;
    }
    fVram = fLfbRange->phys_mem;
    fVramPages = fVramSize >> DEVRAM_PAGE_SIZE_LOG2;

    fRegRange = mem_map->RegisterDevice(0, BANSHEE_REG_BAR_SIZE, &fRegIo,
                                        DEVIO_SIZE8 | DEVIO_SIZE16 |
                                        DEVIO_SIZE32 | DEVIO_DISABLED);
    pci_register_bar(fPciDev, 0, BANSHEE_REG_BAR_SIZE,
                     PCI_ADDRESS_SPACE_MEM, this);
    pci_register_bar(fPciDev, 1, BANSHEE_LFB_BAR_SIZE,
                     PCI_ADDRESS_SPACE_MEM, this);

    /* The same registers answer in port space, which is how a driver that
       cannot map the aperture reaches them. */
    if (port_map != nullptr) {
        fPortRange = port_map->RegisterDevice(0, BANSHEE_IO_BAR_SIZE,
                                              &fPortIo,
                                              DEVIO_SIZE8 | DEVIO_SIZE16 |
                                              DEVIO_SIZE32 | DEVIO_DISABLED);
        pci_register_bar(fPciDev, 2, BANSHEE_IO_BAR_SIZE,
                         PCI_ADDRESS_SPACE_IO, this);
    }

    fIrq = pci_device_get_irq(fPciDev, 0);

    Reset();

    /* The window opens at the configured size and shows nothing until a
       driver turns the video processor on; the mode it then sets is what
       resizes the screen. */
    fb_size = fVramSize;
    fb_data = nullptr;
    width = fInitWidth;
    height = fInitHeight;
    stride = 0;

    fCtx->screen = this;
    fCtx->fb_dev = this;
    fCtx->screen_width = fInitWidth;
    fCtx->screen_height = fInitHeight;
    return true;
}


void BansheeDevice::Reset()
{
    memset(fIoRegs, 0, sizeof(fIoRegs));
    memset(fCmdRegs, 0, sizeof(fCmdRegs));
    memset(f2dRegs, 0, sizeof(f2dRegs));
    memset(fPattern, 0, sizeof(fPattern));
    memset(fPalette, 0, sizeof(fPalette));
    fDacIndex = 0;
    fPaletteDirty = true;
    fHostActive = false;
    fHostData.clear();
    fModeFormat = -1;
    fFullUpdate = true;
    fSeenCommands = 0;
    if (fIrq != nullptr && fIrqLevel) {
        fIrqLevel = false;
        fIrq->Set(0);
    }
}


void BansheeDevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    switch (bar_num) {
    case 0:
        if (fRegRange != nullptr) {
            fRegRange->SetAddr(addr, enabled);
        }
        break;
    case 1:
        if (fLfbRange != nullptr) {
            pci_device_get_mem_map(fPciDev)->SetRamAddr(fLfbRange, addr,
                                                        enabled);
        }
        break;
    case 2:
        if (fPortRange != nullptr) {
            fPortRange->SetAddr(addr, enabled);
        }
        break;
    }
}


//#pragma mark - interrupts

void BansheeDevice::UpdateIrq()
{
    uint32_t ctrl = f2dRegs[BLT_INTRCTRL];
    bool level = (get_bits(ctrl, 0, 6) & get_bits(ctrl, 6, 6)) != 0;

    /* A driver that means to poll turns the pin off in configuration space
       rather than in the card. */
    if (fPciDev != nullptr &&
        (pci_device_get_config(fPciDev, PCI_COMMAND, 1) &
         PCI_COMMAND_INTX_DISABLE) != 0) {
        level = false;
    }
    /* Bit 31 reads as the pin, which is active low. */
    f2dRegs[BLT_INTRCTRL] = set_bit(ctrl, 31, !level);
    if (fIrq != nullptr && level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


//#pragma mark - video memory

uint32_t BansheeDevice::ReadPixel(uint32_t addr, int bpp) const
{
    int bytes = bpp / 8;

    if (addr + bytes > fVramSize) {
        return 0;
    }
    uint32_t val = 0;
    for (int i = 0; i < bytes; i++) {
        val |= (uint32_t)fVram[addr + i] << (i * 8);
    }
    return val;
}


void BansheeDevice::WritePixel(uint32_t addr, uint32_t val, int bpp)
{
    int bytes = bpp / 8;

    if (addr + bytes > fVramSize) {
        return;
    }
    for (int i = 0; i < bytes; i++) {
        fVram[addr + i] = val >> (i * 8);
    }
}


/* The refresh finds what the engine drew the same way it finds what the guest
   drew, so anything written here has to leave the same mark a guest write
   would. */
void BansheeDevice::MarkDirty(uint32_t addr, uint32_t len)
{
    if (fLfbRange == nullptr || addr >= fVramSize) {
        return;
    }
    if (len > fVramSize - addr) {
        len = fVramSize - addr;
    }
    uint32_t last = addr + len - 1;
    for (uint32_t page = addr & ~(uint32_t)(DEVRAM_PAGE_SIZE - 1);
         page <= last; page += DEVRAM_PAGE_SIZE) {
        fLfbRange->SetDirtyBit(page);
    }
}


//#pragma mark - the 2D engine

/* Everything one operation needs, read out of the register file once. */
bool BansheeDevice::GatherOp(BltOp *op)
{
    uint32_t cmd = f2dRegs[BLT_COMMAND];
    uint32_t extra = f2dRegs[BLT_COMMANDEXTRA];

    op->mode = get_bits(cmd, BLT_COMMAND_MODE_SHIFT, 4);
    op->src_base = get_bits(f2dRegs[BLT_SRCBASEADDR], 0, 24);
    op->dst_base = get_bits(f2dRegs[BLT_DSTBASEADDR], 0, 24);
    op->src_stride = get_bits(f2dRegs[BLT_SRCFORMAT], 0,
                              BLT_FORMAT_STRIDE_BITS);
    op->dst_stride = get_bits(f2dRegs[BLT_DSTFORMAT], 0,
                              BLT_FORMAT_STRIDE_BITS);
    op->src_format = get_bits(f2dRegs[BLT_SRCFORMAT], BLT_FORMAT_SRC_SHIFT, 4);
    op->dst_format = get_bits(f2dRegs[BLT_DSTFORMAT], BLT_FORMAT_DST_SHIFT, 3);
    op->src_bpp = format_bpp(op->src_format);
    op->dst_bpp = format_bpp(op->dst_format);
    if (op->dst_bpp == 0) {
        banshee_trace("banshee: destination format %d is not one this draws\n",
                      op->dst_format);
        return false;
    }

    op->src_x = sign_extend(get_bits(f2dRegs[BLT_SRCXY], BLT_XY_X_SHIFT,
                                     BLT_XY_BITS), BLT_XY_BITS);
    op->src_y = sign_extend(get_bits(f2dRegs[BLT_SRCXY], BLT_XY_Y_SHIFT,
                                     BLT_XY_BITS), BLT_XY_BITS);
    op->dst_x = sign_extend(get_bits(f2dRegs[BLT_DSTXY], BLT_XY_X_SHIFT,
                                     BLT_XY_BITS), BLT_XY_BITS);
    op->dst_y = sign_extend(get_bits(f2dRegs[BLT_DSTXY], BLT_XY_Y_SHIFT,
                                     BLT_XY_BITS), BLT_XY_BITS);
    op->width = get_bits(f2dRegs[BLT_DSTSIZE], BLT_XY_X_SHIFT, BLT_XY_BITS);
    op->height = get_bits(f2dRegs[BLT_DSTSIZE], BLT_XY_Y_SHIFT, BLT_XY_BITS);

    int clip = (cmd & BLT_COMMAND_CLIP_SELECT) != 0 ? BLT_CLIP1MIN
                                                    : BLT_CLIP0MIN;
    uint32_t clip_min = f2dRegs[clip];
    uint32_t clip_max = f2dRegs[clip + 1];
    op->clip_x0 = get_bits(clip_min, BLT_CLIP_X_SHIFT, BLT_CLIP_BITS);
    op->clip_y0 = get_bits(clip_min, BLT_CLIP_Y_SHIFT, BLT_CLIP_BITS);
    op->clip_x1 = get_bits(clip_max, BLT_CLIP_X_SHIFT, BLT_CLIP_BITS);
    op->clip_y1 = get_bits(clip_max, BLT_CLIP_Y_SHIFT, BLT_CLIP_BITS);

    op->fore = f2dRegs[BLT_COLORFORE];
    op->back = f2dRegs[BLT_COLORBACK];
    op->rop[0] = get_bits(cmd, BLT_COMMAND_ROP0_SHIFT, 8);
    op->rop[1] = get_bits(f2dRegs[BLT_ROP], 0, 8);
    op->rop[2] = get_bits(f2dRegs[BLT_ROP], 8, 8);
    op->rop[3] = get_bits(f2dRegs[BLT_ROP], 16, 8);

    op->transparent = (cmd & BLT_COMMAND_TRANSPARENT) != 0;
    op->mono_pattern = (cmd & BLT_COMMAND_MONO_PATTERN) != 0;
    op->pattern_row0 = (extra & BLT_EXTRA_PATTERN_ROW0) != 0;
    op->pat_x = get_bits(cmd, BLT_COMMAND_PAT_X_SHIFT, 3);
    op->pat_y = get_bits(cmd, BLT_COMMAND_PAT_Y_SHIFT, 3);
    op->x_reverse = (cmd & BLT_COMMAND_X_RIGHT_TO_LEFT) != 0;
    op->y_reverse = (cmd & BLT_COMMAND_Y_BOTTOM_TO_TOP) != 0;

    op->src_colorkey = (extra & BLT_EXTRA_SRC_COLORKEY) != 0;
    op->dst_colorkey = (extra & BLT_EXTRA_DST_COLORKEY) != 0;
    op->src_key_min = get_bits(f2dRegs[BLT_SRCCOLORKEYMIN], 0, 24);
    op->src_key_max = get_bits(f2dRegs[BLT_SRCCOLORKEYMAX], 0, 24);
    op->dst_key_min = get_bits(f2dRegs[BLT_DSTCOLORKEYMIN], 0, 24);
    op->dst_key_max = get_bits(f2dRegs[BLT_DSTCOLORKEYMAX], 0, 24);
    return true;
}


/* The pattern pixel that lands on a destination pixel. 'opaque' comes back
   false for a monochrome pattern bit the transparency setting says to leave
   alone. */
uint32_t BansheeDevice::PatternPixel(const BltOp &op, int x, int y,
                                     bool *opaque) const
{
    int px = (x + op.pat_x) & 7;
    int py = op.pattern_row0 ? 0 : ((y + op.pat_y) & 7);

    *opaque = true;
    if (op.mono_pattern) {
        /* Two registers hold the eight rows, four to a register, with the
           leftmost pixel of a row in the most significant bit of its byte. */
        uint32_t word = fPattern[py >> 2];
        bool bit = get_bit(word, (py & 3) * 8 + (7 - px));
        if (!bit && op.transparent) {
            *opaque = false;
        }
        return bit ? op.fore : op.back;
    }

    /* A colour pattern is packed in the destination format. */
    int index = py * 8 + px;
    int per_word = 32 / op.dst_bpp;
    if (per_word < 1) {
        per_word = 1;
    }
    if (op.dst_bpp == 24) {
        uint32_t byte_off = (uint32_t)index * 3;
        uint32_t word = fPattern[(byte_off >> 2) & (BLT_PATTERN_COUNT - 1)];
        uint32_t next = fPattern[((byte_off >> 2) + 1) &
                                 (BLT_PATTERN_COUNT - 1)];
        uint64_t pair = ((uint64_t)next << 32) | word;
        return (uint32_t)(pair >> ((byte_off & 3) * 8)) & 0xffffff;
    }
    uint32_t word = fPattern[(index / per_word) & (BLT_PATTERN_COUNT - 1)];
    return get_bits(word, (index % per_word) * op.dst_bpp, op.dst_bpp);
}


/* One destination pixel, clipped, combined with the pattern and the
   destination by whichever raster operation the colorkey tests select. */
void BansheeDevice::DrawPixel(const BltOp &op, int x, int y, uint32_t src,
                              bool src_valid)
{
    if (x < op.clip_x0 || x >= op.clip_x1 ||
        y < op.clip_y0 || y >= op.clip_y1) {
        return;
    }
    if (!src_valid) {
        return; /* a transparent monochrome source leaves the pixel alone */
    }

    bool pat_opaque;
    uint32_t pat = PatternPixel(op, x, y, &pat_opaque);
    if (!pat_opaque) {
        return;
    }

    uint32_t addr = op.dst_base + (uint32_t)y * op.dst_stride +
        (uint32_t)x * (op.dst_bpp / 8);
    uint32_t dst = ReadPixel(addr, op.dst_bpp);

    int index = 0;
    if (op.src_colorkey &&
        colorkey_passes(src, op.src_key_min, op.src_key_max, op.dst_bpp)) {
        index |= 2;
    }
    if (op.dst_colorkey &&
        colorkey_passes(dst, op.dst_key_min, op.dst_key_max, op.dst_bpp)) {
        index |= 1;
    }
    WritePixel(addr, apply_rop(op.rop[index], pat, src, dst), op.dst_bpp);
}


void BansheeDevice::ScreenBlt(const BltOp &op)
{
    int xstep = op.x_reverse ? -1 : 1;
    int ystep = op.y_reverse ? -1 : 1;
    bool convert = op.src_format != op.dst_format;

    for (int i = 0; i < op.height; i++) {
        int sy = op.src_y + i * ystep;
        int dy = op.dst_y + i * ystep;
        if (dy < op.clip_y0 || dy >= op.clip_y1) {
            continue;
        }
        for (int j = 0; j < op.width; j++) {
            int sx = op.src_x + j * xstep;
            int dx = op.dst_x + j * xstep;
            uint32_t addr = op.src_base + (uint32_t)sy * op.src_stride +
                (uint32_t)sx * (op.src_bpp / 8);
            uint32_t src = ReadPixel(addr, op.src_bpp);
            if (convert) {
                src = rgb_to_pixel(pixel_to_rgb(src, op.src_format,
                                                fPalette32),
                                   op.dst_format);
            }
            DrawPixel(op, dx, dy, src, true);
        }
        MarkDirty(op.dst_base + (uint32_t)dy * op.dst_stride,
                  (uint32_t)op.width * (op.dst_bpp / 8) + op.dst_bpp / 8);
    }
}


void BansheeDevice::RectFill(const BltOp &op)
{
    for (int i = 0; i < op.height; i++) {
        int dy = op.dst_y + i;
        if (dy < op.clip_y0 || dy >= op.clip_y1) {
            continue;
        }
        for (int j = 0; j < op.width; j++) {
            DrawPixel(op, op.dst_x + j, dy, op.fore, true);
        }
        MarkDirty(op.dst_base + (uint32_t)dy * op.dst_stride +
                  (uint32_t)op.dst_x * (op.dst_bpp / 8),
                  (uint32_t)op.width * (op.dst_bpp / 8) + op.dst_bpp / 8);
    }
}


/* A solid line from srcXY to dstXY. Line styles are not modelled, so a
   stippled line is drawn solid. */
void BansheeDevice::DrawLine(const BltOp &op, bool skip_last)
{
    int x0 = op.src_x, y0 = op.src_y;
    int x1 = op.dst_x, y1 = op.dst_y;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 == x1 && y0 == y1) {
            if (skip_last) {
                break;
            }
        }
        DrawPixel(op, x0, y0, op.fore, true);
        MarkDirty(op.dst_base + (uint32_t)y0 * op.dst_stride +
                  (uint32_t)x0 * (op.dst_bpp / 8), op.dst_bpp / 8);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int err2 = err * 2;
        if (err2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (err2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}


//#pragma mark - host to screen blts

void BansheeDevice::HostBltStart(const BltOp &op)
{
    fHostOp = op;
    fHostData.clear();
    fHostSpan = 0;
    fHostActive = op.width > 0 && op.height > 0 && op.src_bpp > 0;

    /* The source stride is the one the register holds unless the packing
       bits say to derive it from the width of the blt. */
    int packing = get_bits(f2dRegs[BLT_SRCFORMAT], BLT_FORMAT_PACKING_SHIFT,
                           2);
    int bits = fHostOp.width * fHostOp.src_bpp;
    switch (packing) {
    case 1: fHostOp.src_stride = (bits + 7) / 8; break;
    case 2: fHostOp.src_stride = ((bits + 15) / 16) * 2; break;
    case 3: fHostOp.src_stride = ((bits + 31) / 32) * 4; break;
    default: break;
    }
    if (fHostOp.src_stride <= 0) {
        fHostActive = false;
    }
}


/* Draw the span the stream has reached, if all of its bits have arrived. */
bool BansheeDevice::HostBltSpan()
{
    const BltOp &op = fHostOp;
    /* The first pixel's alignment within the stream is what srcXY holds, and
       each span after the first begins one stride further on. */
    int align = op.src_bpp == 1 ? (op.src_x & 0x1f) : (op.src_x & 3) * 8;
    int64_t first_bit = align + (int64_t)fHostSpan * op.src_stride * 8;
    int64_t last_bit = first_bit + (int64_t)op.width * op.src_bpp;

    if ((int64_t)fHostData.size() * 8 < last_bit) {
        return false;
    }

    int dy = op.dst_y + (op.y_reverse ? -fHostSpan : fHostSpan);
    for (int j = 0; j < op.width; j++) {
        int64_t bit = first_bit + (int64_t)j * op.src_bpp;
        uint32_t src;
        bool valid = true;
        if (op.src_bpp == 1) {
            /* The leftmost pixel of a byte is its most significant bit. */
            bool set = get_bit(fHostData[bit >> 3], 7 - (bit & 7));
            src = set ? op.fore : op.back;
            valid = set || !op.transparent;
        } else {
            uint32_t off = (uint32_t)(bit >> 3);
            src = 0;
            for (int b = 0; b < op.src_bpp / 8; b++) {
                src |= (uint32_t)fHostData[off + b] << (b * 8);
            }
            if (op.src_format != op.dst_format) {
                src = rgb_to_pixel(pixel_to_rgb(src, op.src_format,
                                                fPalette32),
                                   op.dst_format);
            }
        }
        DrawPixel(op, op.dst_x + j, dy, src, valid);
    }
    MarkDirty(op.dst_base + (uint32_t)dy * op.dst_stride +
              (uint32_t)op.dst_x * (op.dst_bpp / 8),
              (uint32_t)op.width * (op.dst_bpp / 8) + op.dst_bpp / 8);

    fHostSpan++;
    if (fHostSpan >= op.height) {
        fHostActive = false;
        fHostData.clear();
    }
    return true;
}


void BansheeDevice::HostBltData(uint32_t val)
{
    if (!fHostActive) {
        return; /* data past the end of the blt is discarded */
    }
    for (int i = 0; i < 4; i++) {
        fHostData.push_back(val >> (i * 8));
    }
    while (fHostActive && HostBltSpan()) {
    }
}


//#pragma mark - running a command

void BansheeDevice::Run2D(bool from_launch, uint32_t launch)
{
    BltOp op;

    if (!GatherOp(&op)) {
        return;
    }

    /* A launch write carries the coordinate the mode takes from it. */
    if (from_launch) {
        int x = sign_extend(get_bits(launch, BLT_XY_X_SHIFT, BLT_XY_BITS),
                            BLT_XY_BITS);
        int y = sign_extend(get_bits(launch, BLT_XY_Y_SHIFT, BLT_XY_BITS),
                            BLT_XY_BITS);
        switch (op.mode) {
        case BLT_CMD_SCREEN_BLT:
        case BLT_CMD_SCREEN_STRETCH:
            f2dRegs[BLT_SRCXY] = launch;
            op.src_x = x;
            op.src_y = y;
            break;
        case BLT_CMD_RECT_FILL:
        case BLT_CMD_LINE:
        case BLT_CMD_POLYLINE:
            f2dRegs[BLT_DSTXY] = launch;
            op.dst_x = x;
            op.dst_y = y;
            break;
        default:
            break;
        }
    }

    /* Reported the first time the guest asks for a mode rather than on every
       operation, which is enough to tell what a driver is using the engine
       for without drowning out everything else. */
    if (!get_bit(fSeenCommands, op.mode)) {
        fSeenCommands = set_bit(fSeenCommands, op.mode, true);
        banshee_trace("banshee: 2D command %d: dst %d,%d %dx%d src %d,%d "
                      "rop %02x fmt %d->%d%s%s\n", op.mode, op.dst_x,
                      op.dst_y, op.width, op.height, op.src_x, op.src_y,
                      op.rop[0], op.src_format, op.dst_format,
                      op.transparent ? " transparent" : "",
                      op.mono_pattern ? " mono-pattern" : "");
    }

    switch (op.mode) {
    case BLT_CMD_NOP:
        break;
    case BLT_CMD_SCREEN_BLT:
        ScreenBlt(op);
        break;
    case BLT_CMD_RECT_FILL:
        RectFill(op);
        break;
    case BLT_CMD_LINE:
    case BLT_CMD_POLYLINE:
        DrawLine(op, op.mode == BLT_CMD_POLYLINE);
        /* The endpoint becomes the start of the next segment. */
        f2dRegs[BLT_SRCXY] = f2dRegs[BLT_DSTXY];
        break;
    case BLT_CMD_HOST_BLT:
        HostBltStart(op);
        break;
    case BLT_CMD_SGRAM_MODE:
    case BLT_CMD_SGRAM_MASK:
    case BLT_CMD_SGRAM_COLOR:
        /* Block write modes of the memory the card is built with, which
           nothing here models. */
        break;
    default:
        banshee_trace("banshee: 2D command %d is not one this draws\n",
                      op.mode);
        break;
    }

    /* The destination walks on by its own size when the command asks. */
    uint32_t cmd = f2dRegs[BLT_COMMAND];
    if ((cmd & (BLT_COMMAND_INC_DST_X | BLT_COMMAND_INC_DST_Y)) != 0 &&
        (op.mode == BLT_CMD_SCREEN_BLT || op.mode == BLT_CMD_RECT_FILL)) {
        int x = op.dst_x, y = op.dst_y;
        if ((cmd & BLT_COMMAND_INC_DST_X) != 0) {
            x += op.width;
        }
        if ((cmd & BLT_COMMAND_INC_DST_Y) != 0) {
            y += op.height;
        }
        f2dRegs[BLT_DSTXY] =
            set_bits(set_bits(0u, BLT_XY_X_SHIFT, BLT_XY_BITS, x),
                     BLT_XY_Y_SHIFT, BLT_XY_BITS, y);
    }
}


void BansheeDevice::LaunchWrite(uint32_t val)
{
    int mode = get_bits(f2dRegs[BLT_COMMAND], BLT_COMMAND_MODE_SHIFT, 4);

    if (mode == BLT_CMD_HOST_BLT || mode == BLT_CMD_HOST_STRETCH) {
        HostBltData(val);
        return;
    }
    Run2D(true, val);
}


//#pragma mark - registers

uint32_t BansheeDevice::StatusRead()
{
    uint32_t val = 0;

    /* Nothing here is ever queued or in flight: the FIFO is always empty and
       idle, and every engine reports itself idle, which is what lets a
       driver's wait loops fall straight through. */
    val = set_bits(val, STATUS_FIFO_FREE_SHIFT, STATUS_FIFO_FREE_BITS,
                   bit_mask(STATUS_FIFO_FREE_BITS));
    /* Toggled on every read so that a driver waiting for a retrace to begin
       and then to end makes progress either way. */
    fRetrace = !fRetrace;
    val = set_bit(val, 6, !fRetrace);
    return val;
}


uint32_t BansheeDevice::IoRegRead(uint32_t offset)
{
    switch (offset) {
    case IO_STATUS:
        return StatusRead();
    case IO_VIDCURRENTLINE:
        /* Walked on every read, which is all a driver watching the beam
           needs to see it move. */
        fIoRegs[IO_VIDCURRENTLINE / 4] =
            (fIoRegs[IO_VIDCURRENTLINE / 4] + 1) & bit_mask(11);
        return fIoRegs[IO_VIDCURRENTLINE / 4];
    case IO_DACDATA:
        return fPalette[fDacIndex];
    default:
        return fIoRegs[offset / 4];
    }
}


void BansheeDevice::DacDataWrite(uint32_t val)
{
    fPalette[fDacIndex] = val & 0xffffff;
    fPaletteDirty = true;
    /* The address register walks on after each entry, which is how a driver
       loads the whole table with one address write. */
    fDacIndex++;
    fIoRegs[IO_DACADDR / 4] = fDacIndex;
}


void BansheeDevice::IoRegWrite(uint32_t offset, uint32_t val)
{
    switch (offset) {
    case IO_STATUS:
        /* A write clears whatever interrupt the card raised. */
        f2dRegs[BLT_INTRCTRL] &= ~INTRCTRL_STATUS_MASK;
        UpdateIrq();
        return;
    case IO_DACADDR:
        fDacIndex = val;
        fIoRegs[offset / 4] = val & 0x1ff;
        return;
    case IO_DACDATA:
        DacDataWrite(val);
        return;
    case IO_VIDPROCCFG:
        fIoRegs[offset / 4] = val;
        banshee_trace("banshee: vidProcCfg %08x\n", val);
        return;
    default:
        fIoRegs[offset / 4] = val;
        return;
    }
}


/* Whether an offset in the command block is one of the two FIFOs' copies of
   'reg', which sit a fixed distance apart. */
static bool cmd_fifo_reg(uint32_t offset, uint32_t reg)
{
    return offset == CMD_FIFO0_BASE + reg || offset == CMD_FIFO1_BASE + reg;
}


uint32_t BansheeDevice::CmdRegRead(uint32_t offset)
{
    /* Both FIFOs report themselves idle and fault free, which is all their
       status registers ever say here. */
    if (cmd_fifo_reg(offset, CMD_FIFO_STATUS)) {
        return 0;
    }
    return fCmdRegs[offset / 4];
}


void BansheeDevice::CmdRegWrite(uint32_t offset, uint32_t val)
{
    /* The registers that start something rather than hold something. With no
       FIFO fetched from and no AGP to move data over, there is nothing for
       either to start. */
    if (offset == CMD_AGPMOVECMD || cmd_fifo_reg(offset, CMD_FIFO_BUMP) ||
        cmd_fifo_reg(offset, CMD_FIFO_STATUS)) {
        return;
    }
    fCmdRegs[offset / 4] = val;
}


uint32_t BansheeDevice::Reg2DRead(uint32_t offset)
{
    uint32_t index = offset / 4;

    if (index >= BLT_PATTERN_FIRST) {
        return fPattern[index - BLT_PATTERN_FIRST];
    }
    if (index >= BLT_LAUNCH_FIRST) {
        return 0; /* the launch area initiates, it does not hold */
    }
    if (index == BLT_STATUS) {
        return StatusRead();
    }
    return f2dRegs[index];
}


void BansheeDevice::Reg2DWrite(uint32_t offset, uint32_t val)
{
    uint32_t index = offset / 4;

    if (index >= BLT_PATTERN_FIRST) {
        fPattern[index - BLT_PATTERN_FIRST] = val;
        return;
    }
    if (index >= BLT_LAUNCH_FIRST) {
        LaunchWrite(val);
        return;
    }

    switch (index) {
    case BLT_STATUS:
        f2dRegs[BLT_INTRCTRL] &= ~INTRCTRL_STATUS_MASK;
        UpdateIrq();
        return;
    case BLT_INTRCTRL:
        /* The generated flags are cleared by writing a zero to them, so the
           value written is taken as it stands and the pin recomputed. */
        f2dRegs[index] = val;
        UpdateIrq();
        return;
    case BLT_PATTERN0ALIAS:
        fPattern[0] = val;
        return;
    case BLT_PATTERN1ALIAS:
        fPattern[1] = val;
        return;
    case BLT_COMMAND:
        f2dRegs[index] = val;
        if ((val & BLT_COMMAND_INITIATE) != 0) {
            Run2D(false, 0);
        } else if (get_bits(val, BLT_COMMAND_MODE_SHIFT, 4) ==
                   BLT_CMD_HOST_BLT) {
            /* The data still arrives through the launch area, so the blt is
               set up here either way. */
            BltOp op;
            if (GatherOp(&op)) {
                HostBltStart(op);
            }
        }
        return;
    default:
        f2dRegs[index] = val;
        return;
    }
}


/* The register aperture. Only the blocks this models decode; a read of
   anything else gives back zero rather than stale data. */
uint32_t BansheeDevice::RegRead(uint32_t offset, int size_log2)
{
    uint32_t val;

    if (offset < BANSHEE_IO_BAR_SIZE) {
        val = IoRegRead(offset & ~3u);
    } else if (offset >= BANSHEE_REG_CMD_BASE &&
               offset < BANSHEE_REG_CMD_BASE + CMD_REG_SIZE) {
        val = CmdRegRead((offset - BANSHEE_REG_CMD_BASE) & ~3u);
    } else if (offset >= BANSHEE_REG_2D_BASE && offset < BANSHEE_REG_2D_END) {
        uint32_t off = (offset - BANSHEE_REG_2D_BASE) & ~3u;
        if (off >= (BLT_PATTERN_FIRST + BLT_PATTERN_COUNT) * 4) {
            return 0;
        }
        val = Reg2DRead(off);
    } else if (offset >= BANSHEE_REG_3D_BASE && offset < BANSHEE_REG_3D_END) {
        switch ((offset - BANSHEE_REG_3D_BASE) & ~3u) {
        case REG3D_STATUS:   val = StatusRead(); break;
        case REG3D_INTRCTRL: val = f2dRegs[BLT_INTRCTRL]; break;
        default:             return 0;
        }
    } else {
        banshee_trace("banshee: read of register %06x\n", offset);
        return 0;
    }
    return val >> ((offset & 3) * 8);
}


void BansheeDevice::RegWrite(uint32_t offset, uint32_t val, int size_log2)
{
    /* Every register here is a word and a driver writes whole words; a
       narrower write would need the word read back first, which no driver
       asks for. */
    if (size_log2 != 2) {
        banshee_trace("banshee: %d bit write of register %06x\n",
                      8 << size_log2, offset);
        return;
    }
    if (offset < BANSHEE_IO_BAR_SIZE) {
        IoRegWrite(offset & ~3u, val);
    } else if (offset >= BANSHEE_REG_CMD_BASE &&
               offset < BANSHEE_REG_CMD_BASE + CMD_REG_SIZE) {
        CmdRegWrite((offset - BANSHEE_REG_CMD_BASE) & ~3u, val);
    } else if (offset >= BANSHEE_REG_2D_BASE && offset < BANSHEE_REG_2D_END) {
        uint32_t off = (offset - BANSHEE_REG_2D_BASE) & ~3u;
        if (off < (BLT_PATTERN_FIRST + BLT_PATTERN_COUNT) * 4) {
            Reg2DWrite(off, val);
        }
    } else if (offset >= BANSHEE_REG_3D_BASE && offset < BANSHEE_REG_3D_END) {
        switch ((offset - BANSHEE_REG_3D_BASE) & ~3u) {
        case REG3D_STATUS:
            f2dRegs[BLT_INTRCTRL] &= ~INTRCTRL_STATUS_MASK;
            UpdateIrq();
            break;
        case REG3D_INTRCTRL:
            f2dRegs[BLT_INTRCTRL] = val;
            UpdateIrq();
            break;
        default:
            /* Nothing else in this block is modelled: there is no 3D
               engine, so a command written here draws nothing. */
            banshee_trace("banshee: 3D register %06x = %08x\n", offset, val);
            break;
        }
    } else {
        banshee_trace("banshee: write of register %06x = %08x\n", offset, val);
    }
}


uint32_t BansheeDevice::PortRead(uint32_t offset, int size_log2)
{
    uint32_t val = IoRegRead(offset & ~3u);
    return val >> ((offset & 3) * 8);
}


void BansheeDevice::PortWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (size_log2 != 2) {
        banshee_trace("banshee: %d bit write of port %02x\n",
                      8 << size_log2, offset);
        return;
    }
    IoRegWrite(offset & ~3u, val);
}


//#pragma mark - the display

/* The palette as host pixels. Returns whether anything changed. */
bool BansheeDevice::UpdatePalette()
{
    if (!fPaletteDirty) {
        return false;
    }
    fPaletteDirty = false;

    bool changed = false;
    int base = get_bit(fIoRegs[IO_VIDPROCCFG / 4], 12) ? 256 : 0;
    for (int i = 0; i < 256; i++) {
        uint32_t val = fPalette[base + i] & 0xffffff;
        if (fPalette32[i] != val) {
            fPalette32[i] = val;
            changed = true;
        }
    }
    return changed;
}


/* The mode the video processor's registers describe, sizing the picture the
   screen is handed and the shadow it is drawn into. */
void BansheeDevice::ApplyMode()
{
    uint32_t cfg = fIoRegs[IO_VIDPROCCFG / 4];
    int new_width = 0, new_height = 0, new_format = -1;
    uint32_t new_start = 0;
    int new_stride = 0;

    if ((cfg & VIDPROCCFG_ENABLE) != 0 && (cfg & VIDPROCCFG_DESKTOP_EN) != 0) {
        uint32_t size = fIoRegs[IO_VIDSCREENSIZE / 4];
        new_width = get_bits(size, 0, 12);
        new_height = get_bits(size, 12, 12);
        new_format = get_bits(cfg, VIDPROCCFG_DESKTOP_FORMAT_SHIFT, 3);
        new_start = get_bits(fIoRegs[IO_VIDDESKTOPSTARTADDR / 4], 0, 24);
        new_stride = get_bits(fIoRegs[IO_VIDDESKTOPOVERLAYSTRIDE / 4], 0, 15);
        if (new_width <= 0 || new_height <= 0 || new_stride <= 0 ||
            new_format > VIDPROC_FORMAT_RGB32) {
            new_format = -1;
        } else if ((uint64_t)new_start +
                   (uint64_t)new_height * new_stride > fVramSize) {
            /* A desktop that does not fit the memory the card has is one no
               refresh could read. */
            new_format = -1;
        }
    }

    if (new_format == fModeFormat && new_width == fModeWidth &&
        new_height == fModeHeight && new_start == fModeStart &&
        new_stride == fModeStride) {
        return;
    }

    fModeFormat = new_format;
    fModeWidth = new_width;
    fModeHeight = new_height;
    fModeStart = new_start;
    fModeStride = new_stride;
    fFullUpdate = true;

    if (new_format < 0) {
        banshee_trace("banshee: no mode\n");
        fb_data = nullptr;
        fShadow.clear();
        return;
    }

    banshee_trace("banshee: mode %dx%d format %d stride %d start %06x\n",
                  new_width, new_height, new_format, new_stride, new_start);
    width = new_width;
    height = new_height;
    if (new_format == VIDPROC_FORMAT_RGB32) {
        /* The guest's pixels are what the screen takes: no copy. */
        stride = new_stride;
        fb_data = fVram + new_start;
        fShadow.clear();
        return;
    }
    stride = width * 4;
    fShadow.assign((size_t)stride * height, 0);
    fb_data = fShadow.data();
}


void BansheeDevice::DrawRows(int y0, int y1)
{
    if (fModeFormat == VIDPROC_FORMAT_RGB32) {
        return;
    }

    const uint8_t *src = fVram + fModeStart + (size_t)y0 * fModeStride;
    uint8_t *dst = fShadow.data() + (size_t)y0 * stride;

    for (int y = y0; y < y1; y++) {
        uint32_t *out = (uint32_t *)dst;
        switch (fModeFormat) {
        case VIDPROC_FORMAT_8BPP:
            for (int x = 0; x < width; x++) {
                out[x] = fPalette32[src[x]];
            }
            break;
        case VIDPROC_FORMAT_RGB565:
            for (int x = 0; x < width; x++) {
                uint32_t v = src[x * 2] | ((uint32_t)src[x * 2 + 1] << 8);
                out[x] = pixel_to_rgb(v, BLT_PIXEL_16BPP, fPalette32);
            }
            break;
        case VIDPROC_FORMAT_RGB24:
            for (int x = 0; x < width; x++) {
                out[x] = src[x * 3] | ((uint32_t)src[x * 3 + 1] << 8) |
                    ((uint32_t)src[x * 3 + 2] << 16);
            }
            break;
        }
        src += fModeStride;
        dst += stride;
    }
}


/* The cursor as the screen takes it: two interleaved bitmaps in video memory
   become one image of host pixels. The two bits of a pixel choose between the
   two cursor colours, the screen showing through, and its inverse; nothing
   here can invert what is behind the cursor, so that case is drawn as the
   opposite colour instead. */
void BansheeDevice::RefreshCursor(HostScreen *screen)
{
    uint32_t cfg = fIoRegs[IO_VIDPROCCFG / 4];
    bool show = (cfg & VIDPROCCFG_ENABLE) != 0 &&
        (cfg & VIDPROCCFG_CURSOR_EN) != 0;

    if (!show) {
        if (fCursorShown) {
            fCursorShown = false;
            screen->SetCursor(nullptr, 0, 0, 0, 0);
        }
        return;
    }

    uint32_t pat = get_bits(fIoRegs[IO_HWCURPATADDR / 4], 0, 24);
    uint32_t c0 = get_bits(fIoRegs[IO_HWCURC0 / 4], 0, 24);
    uint32_t c1 = get_bits(fIoRegs[IO_HWCURC1 / 4], 0, 24);
    bool x11 = (cfg & VIDPROCCFG_CURSOR_X11) != 0;
    uint32_t image[BANSHEE_CURSOR_SIZE * BANSHEE_CURSOR_SIZE];

    /* Each line of the patterns is eight bytes of the first followed by
       eight of the second, and the two bits of a pixel choose what it
       shows. */
    for (int y = 0; y < BANSHEE_CURSOR_SIZE; y++) {
        uint32_t row = pat + (uint32_t)y * BANSHEE_CURSOR_STRIDE;
        for (int x = 0; x < BANSHEE_CURSOR_SIZE; x++) {
            uint32_t b0 = 0, b1 = 0;
            if (row + BANSHEE_CURSOR_STRIDE <= fVramSize) {
                b0 = fVram[row + (x >> 3)];
                b1 = fVram[row + 8 + (x >> 3)];
            }
            int bit = 7 - (x & 7);
            int sel = (get_bit(b0, bit) ? 2 : 0) | (get_bit(b1, bit) ? 1 : 0);
            uint32_t pixel;
            if (x11) {
                /* 00 and 01 are the screen; 10 and 11 are the colours. */
                pixel = sel < 2 ? 0 : (0xff000000u | (sel == 2 ? c0 : c1));
            } else {
                /* 00 and 01 are the colours; 10 is the screen and 11 its
                   inverse, which nothing here can draw, so it is shown as
                   the opposite colour. */
                switch (sel) {
                case 0: pixel = 0xff000000u | c0; break;
                case 1: pixel = 0xff000000u | c1; break;
                case 2: pixel = 0; break;
                default: pixel = 0xff000000u | (c0 ^ 0xffffff); break;
                }
            }
            image[y * BANSHEE_CURSOR_SIZE + x] = pixel;
        }
    }

    /* A new shape is written over the old one at the same address, so the
       pixels are what says the cursor changed. */
    if (!fCursorShown || memcmp(image, fCursorImage, sizeof(image)) != 0) {
        memcpy(fCursorImage, image, sizeof(image));
        fCursorShown = true;
        screen->SetCursor(fCursorImage, BANSHEE_CURSOR_SIZE,
                          BANSHEE_CURSOR_SIZE, 0, 0);
    }

    /* hwCurLoc holds the coordinates of the cursor's bottom right pixel, so
       its top left one is a cursor's width and height back from there, less
       the one pixel the corner itself is. */
    uint32_t loc = fIoRegs[IO_HWCURLOC / 4];
    int x = (int)get_bits(loc, 0, 11) - (BANSHEE_CURSOR_SIZE - 1);
    int y = (int)get_bits(loc, 16, 11) - (BANSHEE_CURSOR_SIZE - 1);
    screen->MoveCursor(x, y);
}


void BansheeDevice::Refresh(HostScreen *screen)
{
    /* The vertical sync a driver asked to be told about, which arrives once
       per refresh here rather than once per frame the monitor draws. */
    uint32_t ctrl = f2dRegs[BLT_INTRCTRL];
    if ((ctrl & (INTRCTRL_VSYNC_RISE_EN | INTRCTRL_VSYNC_FALL_EN)) != 0) {
        if ((ctrl & INTRCTRL_VSYNC_RISE_EN) != 0) {
            ctrl |= INTRCTRL_VSYNC_RISE;
        }
        if ((ctrl & INTRCTRL_VSYNC_FALL_EN) != 0) {
            ctrl |= INTRCTRL_VSYNC_FALL;
        }
        f2dRegs[BLT_INTRCTRL] = ctrl;
        UpdateIrq();
    }

    ApplyMode();
    if (fModeFormat < 0) {
        screen->SetFramebuffer(nullptr, width, height, 0);
        return;
    }

    bool redraw = fFullUpdate;
    if (fModeFormat == VIDPROC_FORMAT_8BPP && UpdatePalette()) {
        redraw = true;
    }
    screen->SetFramebuffer(fb_data, width, height, stride);

    if (redraw) {
        /* The dirty bits are taken and dropped: every row is drawn anyway. */
        fLfbRange->DirtyBits();
        DrawRows(0, height);
        screen->Update(0, 0, width, height);
    } else {
        fb_walk_dirty(fLfbRange, fVramPages, fModeStart, fModeStride, height,
                      [&](int y0, int y1) {
            DrawRows(y0, y1);
            screen->Update(0, y0, width, y1 - y0);
        });
    }
    fFullUpdate = false;

    RefreshCursor(screen);
}


//#pragma mark - class

/* A Voodoo Banshee on a PCI bus. "vram" is the video memory in megabytes,
   "model" is "banshee" or "voodoo3", which changes only what a driver matches
   on, and "width" and "height" are the size the window opens at before a
   driver has set a mode. */
class BansheeClass final: public DeviceClass {
public:
    BansheeClass(): DeviceClass("banshee") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override;
};


Device *BansheeClass::Create(const DeviceConfig &cfg, DeviceContext *ctx) const
{
    const char *model;
    int vram_mb, width, height;
    uint16_t device_id;

    if (!cfg.GetStrOpt("model", &model) ||
        !cfg.GetInt("vram", &vram_mb, BANSHEE_DEFAULT_VRAM_MB) ||
        !cfg.GetInt("width", &width, 1024) ||
        !cfg.GetInt("height", &height, 768)) {
        return nullptr;
    }
    if (model == nullptr || strcmp(model, "banshee") == 0) {
        device_id = BANSHEE_PCI_BANSHEE_ID;
    } else if (strcmp(model, "voodoo3") == 0) {
        device_id = BANSHEE_PCI_VOODOO3_ID;
    } else {
        vm_error("banshee: 'model' must be \"banshee\" or \"voodoo3\"\n");
        return nullptr;
    }
    if (vram_mb < BANSHEE_MIN_VRAM_MB || vram_mb > BANSHEE_MAX_VRAM_MB ||
        (vram_mb & (vram_mb - 1)) != 0) {
        vm_error("banshee: 'vram' must be a power of two between %d and %d "
                 "MB\n", BANSHEE_MIN_VRAM_MB, BANSHEE_MAX_VRAM_MB);
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        vm_error("banshee: 'width' and 'height' must be positive\n");
        return nullptr;
    }
    return new BansheeDevice(ctx, cfg.IdOr("banshee"), device_id, vram_mb,
                             width, height);
}

static const BansheeClass sBansheeClass;
