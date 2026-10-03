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
#pragma once

#include <vector>

#include "device.h"
#include "devices.h"
#include "machine.h"
#include "pci.h"

/* What the card reports, and what a driver binds to. A Voodoo3 is the same
   part with a faster clock as far as anything here is concerned. */
#define BANSHEE_PCI_VENDOR_ID   0x121a
#define BANSHEE_PCI_BANSHEE_ID  0x0003
#define BANSHEE_PCI_VOODOO3_ID  0x0005

/* The two memory apertures, both of which a real card decodes at their full
   size whatever it is populated with. */
#define BANSHEE_REG_BAR_SIZE 0x2000000 /* 32 MB */
#define BANSHEE_LFB_BAR_SIZE 0x2000000 /* 32 MB */
/* The I/O register block, which the same registers answer at. */
#define BANSHEE_IO_BAR_SIZE  0x100

/* Regions of the register aperture. */
#define BANSHEE_REG_IO_BASE  0x000000 /* the I/O register block, remapped */
#define BANSHEE_REG_CMD_BASE 0x080000 /* command transfer and AGP */
#define BANSHEE_REG_2D_BASE  0x100000
#define BANSHEE_REG_2D_END   0x200000
/* The 3D register block. No 3D engine is modelled, but its first two
   registers are the same status and intrCtrl the other blocks carry, and a
   driver reads status here to wait for the engine whatever it was using. */
#define BANSHEE_REG_3D_BASE  0x200000
#define BANSHEE_REG_3D_END   0x600000
#define REG3D_STATUS         0x000
#define REG3D_INTRCTRL       0x004

/* Video memory a card was populated with, in megabytes. */
#define BANSHEE_DEFAULT_VRAM_MB 16
#define BANSHEE_MIN_VRAM_MB     4
#define BANSHEE_MAX_VRAM_MB     32

/* The I/O register block, as byte offsets. Only the ones with behaviour are
   named; the rest are held and read back. */
#define IO_STATUS         0x00
#define IO_PCIINIT0       0x04
#define IO_SIPMONITOR     0x08
#define IO_LFBMEMORYCONFIG 0x0c
#define IO_MISCINIT0      0x10
#define IO_MISCINIT1      0x14
#define IO_DRAMINIT0      0x18
#define IO_DRAMINIT1      0x1c
#define IO_AGPINIT        0x20
#define IO_TMUGBEINIT     0x24
#define IO_VGAINIT0       0x28
#define IO_VGAINIT1       0x2c
#define IO_DRAMCOMMAND    0x30
#define IO_DRAMDATA       0x34
#define IO_PLLCTRL0       0x40
#define IO_PLLCTRL1       0x44
#define IO_PLLCTRL2       0x48
#define IO_DACMODE        0x4c
#define IO_DACADDR        0x50
#define IO_DACDATA        0x54
#define IO_RGBMAXDELTA    0x58
#define IO_VIDPROCCFG     0x5c
#define IO_HWCURPATADDR   0x60
#define IO_HWCURLOC       0x64
#define IO_HWCURC0        0x68
#define IO_HWCURC1        0x6c
#define IO_VIDINFORMAT    0x70
#define IO_VIDINSTATUS    0x74
#define IO_VIDSERIALPARALLELPORT 0x78
#define IO_VIDCHROMAMIN   0x8c
#define IO_VIDCHROMAMAX   0x90
#define IO_VIDCURRENTLINE 0x94
#define IO_VIDSCREENSIZE  0x98
#define IO_VIDDESKTOPSTARTADDR 0xe4
#define IO_VIDDESKTOPOVERLAYSTRIDE 0xe8
#define IO_REG_COUNT      (BANSHEE_IO_BAR_SIZE / 4)

/* status, which is the register a driver spins on.
 *
 * The 2D databook gives bits 5:0 as the free space in a 64 entry FIFO, and
 * the Voodoo3 specification's own 2D section corrects that: the count is five
 * bits and bit 5 is a busy flag of its own. A driver that waits for the FIFO
 * spins on that bit, so the wider field would never let one through. */
#define STATUS_FIFO_FREE_SHIFT 0 /* five bits; 0x1f is an empty FIFO */
#define STATUS_FIFO_FREE_BITS  5
#define STATUS_FIFO_BUSY  bit_at(5)
#define STATUS_VRETRACE   bit_at(6)  /* 0 while the retrace is active */
#define STATUS_FBI_BUSY   bit_at(7)
#define STATUS_TREX_BUSY  bit_at(8)
#define STATUS_BUSY       bit_at(9)
#define STATUS_2D_BUSY    bit_at(10)
#define STATUS_CMDFIFO0_BUSY bit_at(11)
#define STATUS_CMDFIFO1_BUSY bit_at(12)

/* intrCtrl: six enables, the matching six generated flags, and the pin. */
#define INTRCTRL_HSYNC_RISE_EN bit_at(0)
#define INTRCTRL_HSYNC_FALL_EN bit_at(1)
#define INTRCTRL_VSYNC_RISE_EN bit_at(2)
#define INTRCTRL_VSYNC_FALL_EN bit_at(3)
#define INTRCTRL_PCIFIFO_EN    bit_at(4)
#define INTRCTRL_USER_EN       bit_at(5)
#define INTRCTRL_HSYNC_RISE    bit_at(6)
#define INTRCTRL_HSYNC_FALL    bit_at(7)
#define INTRCTRL_VSYNC_RISE    bit_at(8)
#define INTRCTRL_VSYNC_FALL    bit_at(9)
#define INTRCTRL_PCIFIFO       bit_at(10)
#define INTRCTRL_USER          bit_at(11)
#define INTRCTRL_STATUS_MASK   field_mask(6, 6)
/* Written as 1 to clear the external pin; reads as the pin, active low. */
#define INTRCTRL_PCI_INTA      bit_at(31)

/* vidProcCfg, the video processor's configuration. */
#define VIDPROCCFG_ENABLE        bit_at(0) /* 0 leaves the VGA core showing */
#define VIDPROCCFG_CURSOR_X11    bit_at(1)
#define VIDPROCCFG_HALF_MODE     bit_at(4)
#define VIDPROCCFG_DESKTOP_EN    bit_at(7)
#define VIDPROCCFG_OVERLAY_EN    bit_at(8)
#define VIDPROCCFG_CLUT_BYPASS   bit_at(10)
#define VIDPROCCFG_DESKTOP_FORMAT_SHIFT 18 /* three bits */
#define VIDPROCCFG_2X_MODE       bit_at(26)
#define VIDPROCCFG_CURSOR_EN     bit_at(27)

/* Desktop pixel formats, as vidProcCfg names them. */
#define VIDPROC_FORMAT_8BPP   0
#define VIDPROC_FORMAT_RGB565 1
#define VIDPROC_FORMAT_RGB24  2
#define VIDPROC_FORMAT_RGB32  3

/* The hardware cursor is always this, and its patterns are two bitmaps
   interleaved a line at a time in a 16 byte stride. */
#define BANSHEE_CURSOR_SIZE   64
#define BANSHEE_CURSOR_STRIDE 16

/* The command transfer and AGP block, as byte offsets into the block at
   BANSHEE_REG_CMD_BASE. Nothing here transfers anything: no command FIFO is
   fetched from and no AGP move is performed, because the 2D engine draws as
   its registers are written. The registers are held so that the setup a
   driver performs reads back as it left it, which is how one checks it is
   talking to the part it thinks it is. */
#define CMD_REG_SIZE  0x200
#define CMD_REG_COUNT (CMD_REG_SIZE / 4)
#define CMD_AGPREQSIZE        0x000
#define CMD_AGPHOSTADDRESSLOW 0x004
#define CMD_AGPHOSTADDRESSHIGH 0x008
#define CMD_AGPGRAPHICSADDRESS 0x00c
#define CMD_AGPGRAPHICSSTRIDE 0x010
#define CMD_AGPMOVECMD        0x014 /* write only */
/* One block per command FIFO, the second a fixed distance after the first. */
#define CMD_FIFO0_BASE   0x020
#define CMD_FIFO1_BASE   0x050
#define CMD_FIFO_SPACING 0x030
#define CMD_FIFO_BASEADDR 0x000
#define CMD_FIFO_BASESIZE 0x004
#define CMD_FIFO_BUMP     0x008 /* write only */
#define CMD_FIFO_RDPTRL   0x00c
#define CMD_FIFO_RDPTRH   0x010
#define CMD_FIFO_AMIN     0x014
#define CMD_FIFO_AMAX     0x01c
#define CMD_FIFO_STATUS   0x020 /* read only */
#define CMD_FIFO_DEPTH    0x024
#define CMD_FIFO_HOLECNT  0x028
#define CMD_FIFOTHRESH    0x080
#define CMD_HOLEINT       0x084
#define CMD_YUVBASEADDRESS 0x100
#define CMD_YUVSTRIDE      0x104

/* The 2D register file, as dword indices into the block at
   BANSHEE_REG_2D_BASE. */
#define BLT_STATUS          0x00
#define BLT_INTRCTRL        0x01
#define BLT_CLIP0MIN        0x02
#define BLT_CLIP0MAX        0x03
#define BLT_DSTBASEADDR     0x04
#define BLT_DSTFORMAT       0x05
#define BLT_SRCCOLORKEYMIN  0x06
#define BLT_SRCCOLORKEYMAX  0x07
#define BLT_DSTCOLORKEYMIN  0x08
#define BLT_DSTCOLORKEYMAX  0x09
#define BLT_BRESERROR0      0x0a
#define BLT_BRESERROR1      0x0b
#define BLT_ROP             0x0c
#define BLT_SRCBASEADDR     0x0d
#define BLT_COMMANDEXTRA    0x0e
#define BLT_LINESTIPPLE     0x0f
#define BLT_LINESTYLE       0x10
#define BLT_PATTERN0ALIAS   0x11
#define BLT_PATTERN1ALIAS   0x12
#define BLT_CLIP1MIN        0x13
#define BLT_CLIP1MAX        0x14
#define BLT_SRCFORMAT       0x15
#define BLT_SRCSIZE         0x16
#define BLT_SRCXY           0x17
#define BLT_COLORBACK       0x18
#define BLT_COLORFORE       0x19
#define BLT_DSTSIZE         0x1a
#define BLT_DSTXY           0x1b
#define BLT_COMMAND         0x1c
#define BLT_REG_COUNT       0x20 /* the launch area begins here */
#define BLT_LAUNCH_FIRST    0x20
#define BLT_LAUNCH_LAST     0x3f
#define BLT_PATTERN_FIRST   0x40
#define BLT_PATTERN_COUNT   0x40

/* command[3:0], the 2D engine's drawing modes. */
#define BLT_CMD_NOP            0
#define BLT_CMD_SCREEN_BLT     1
#define BLT_CMD_SCREEN_STRETCH 2
#define BLT_CMD_HOST_BLT       3
#define BLT_CMD_HOST_STRETCH   4
#define BLT_CMD_RECT_FILL      5
#define BLT_CMD_LINE           6
#define BLT_CMD_POLYLINE       7
#define BLT_CMD_POLYGON        8
#define BLT_CMD_SGRAM_MODE     13
#define BLT_CMD_SGRAM_MASK     14
#define BLT_CMD_SGRAM_COLOR    15

/* command, the rest of it. */
#define BLT_COMMAND_MODE_SHIFT     0 /* four bits */
#define BLT_COMMAND_INITIATE       bit_at(8)
#define BLT_COMMAND_REVERSIBLE     bit_at(9)
#define BLT_COMMAND_INC_DST_X      bit_at(10)
#define BLT_COMMAND_INC_DST_Y      bit_at(11)
#define BLT_COMMAND_STIPPLE_LINE   bit_at(12)
#define BLT_COMMAND_MONO_PATTERN   bit_at(13)
#define BLT_COMMAND_X_RIGHT_TO_LEFT bit_at(14)
#define BLT_COMMAND_Y_BOTTOM_TO_TOP bit_at(15)
#define BLT_COMMAND_TRANSPARENT    bit_at(16)
#define BLT_COMMAND_PAT_X_SHIFT    17 /* three bits */
#define BLT_COMMAND_PAT_Y_SHIFT    20 /* three bits */
#define BLT_COMMAND_CLIP_SELECT    bit_at(23)
#define BLT_COMMAND_ROP0_SHIFT     24 /* eight bits */

/* commandExtra */
#define BLT_EXTRA_SRC_COLORKEY  bit_at(0)
#define BLT_EXTRA_DST_COLORKEY  bit_at(1)
#define BLT_EXTRA_WAIT_VSYNC    bit_at(2)
#define BLT_EXTRA_PATTERN_ROW0  bit_at(3)

/* srcFormat and dstFormat. */
#define BLT_FORMAT_STRIDE_BITS  14
#define BLT_FORMAT_SRC_SHIFT    16 /* four bits */
#define BLT_FORMAT_DST_SHIFT    16 /* three bits */
#define BLT_FORMAT_BYTE_SWIZZLE bit_at(20)
#define BLT_FORMAT_WORD_SWIZZLE bit_at(21)
#define BLT_FORMAT_PACKING_SHIFT 22 /* two bits */

/* The pixel formats the two registers name, which are numbered alike. */
#define BLT_PIXEL_MONO   0
#define BLT_PIXEL_8BPP   1
#define BLT_PIXEL_15BPP  2
#define BLT_PIXEL_16BPP  3
#define BLT_PIXEL_24BPP  4
#define BLT_PIXEL_32BPP  5
#define BLT_PIXEL_YUYV   8
#define BLT_PIXEL_UYVY   9

/* An X or Y field of a coordinate register: thirteen bits at 0 and at 16. */
#define BLT_XY_X_SHIFT 0
#define BLT_XY_Y_SHIFT 16
#define BLT_XY_BITS    13
/* A clip register, whose fields are narrower and unsigned. */
#define BLT_CLIP_X_SHIFT 0
#define BLT_CLIP_Y_SHIFT 16
#define BLT_CLIP_BITS    12


/* The state one 2D operation runs against, gathered from the register file
   when the operation starts so that nothing below re-reads a register. */
struct BltOp {
    int mode = 0;
    uint32_t src_base = 0;
    uint32_t dst_base = 0;
    int src_stride = 0;
    int dst_stride = 0;
    int src_format = 0;
    int dst_format = 0;
    int src_bpp = 0; /* bits, 1 for a monochrome source */
    int dst_bpp = 0;
    int src_x = 0, src_y = 0;
    int dst_x = 0, dst_y = 0;
    int width = 0, height = 0;
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
    uint32_t fore = 0, back = 0;
    uint8_t rop[4] = {};
    bool transparent = false;
    bool mono_pattern = false;
    bool pattern_row0 = false;
    int pat_x = 0, pat_y = 0;
    bool x_reverse = false;
    bool y_reverse = false;
    bool src_colorkey = false;
    bool dst_colorkey = false;
    uint32_t src_key_min = 0, src_key_max = 0;
    uint32_t dst_key_min = 0, dst_key_max = 0;
};


/* A 3dfx Voodoo Banshee, modelled from the register level down to what a
 * driver can observe: the video processor that scans the desktop surface out
 * of video memory, the 2D drawing engine, and the hardware cursor.
 *
 * The VGA core a real Banshee also carries is not modelled. The card shows
 * nothing until a driver turns the video processor on, which is what
 * vidProcCfg bit 0 does; before that the screen is blank.
 *
 * Drawing is synchronous. A command either runs when the command register is
 * written or when the launch area is written, and has finished by the time
 * the write returns, so the engine reports itself idle and the FIFO empty at
 * all times.
 */
class BansheeDevice final: public Device, public FBDevice,
                           public PCIBarTarget {
private:
    DeviceContext *fCtx;
    uint16_t fDeviceId;
    uint32_t fVramSize;
    /* The size the window opens at, until a driver sets a mode of its
       own. */
    int fInitWidth;
    int fInitHeight;

    PCIDevice *fPciDev = nullptr;
    PhysMemoryRange *fRegRange = nullptr;
    PhysMemoryRange *fLfbRange = nullptr;
    PhysMemoryRange *fPortRange = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;

    uint8_t *fVram = nullptr;
    int fVramPages = 0;

    uint32_t fIoRegs[IO_REG_COUNT] {};
    uint32_t fCmdRegs[CMD_REG_COUNT] {};
    uint32_t f2dRegs[BLT_REG_COUNT] {};
    uint32_t fPattern[BLT_PATTERN_COUNT] {};

    /* The RAMDAC palette, as the guest wrote it and as host pixels. */
    uint8_t fDacIndex = 0;
    uint32_t fPalette[512] {};
    uint32_t fPalette32[256] {};
    bool fPaletteDirty = true;

    /* Toggled on every read of the registers that report it, so that a
       driver polling for a retrace makes progress. */
    bool fRetrace = false;

    /* The mode being shown, and the picture it is drawn into when the
       guest's pixels are not what the screen takes. */
    int fModeWidth = 0;
    int fModeHeight = 0;
    int fModeFormat = -1;
    uint32_t fModeStart = 0;
    int fModeStride = 0;
    bool fFullUpdate = true;
    std::vector<uint8_t> fShadow;

    /* The cursor image the screen was last handed. The patterns live in
       video memory, which the guest rewrites in place, so there is nothing
       to watch but the pixels themselves. */
    bool fCursorShown = false;
    uint32_t fCursorImage[BANSHEE_CURSOR_SIZE * BANSHEE_CURSOR_SIZE] {};

    /* One bit per 2D command mode, so that each is reported the first time
       the guest asks for it and not on every operation. */
    uint32_t fSeenCommands = 0;

    /* A host-to-screen blt in progress: the pixel stream the guest is
       writing to the launch area, and which span it has reached. */
    BltOp fHostOp;
    std::vector<uint8_t> fHostData;
    int fHostSpan = 0;
    bool fHostActive = false;

    void Reset();
    void UpdateIrq();

    /* registers */
    uint32_t IoRegRead(uint32_t offset);
    void IoRegWrite(uint32_t offset, uint32_t val);
    uint32_t CmdRegRead(uint32_t offset);
    void CmdRegWrite(uint32_t offset, uint32_t val);
    uint32_t Reg2DRead(uint32_t offset);
    void Reg2DWrite(uint32_t offset, uint32_t val);
    uint32_t StatusRead();
    void DacDataWrite(uint32_t val);

    uint32_t RegRead(uint32_t offset, int size_log2);
    void RegWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t PortRead(uint32_t offset, int size_log2);
    void PortWrite(uint32_t offset, uint32_t val, int size_log2);

    /* video memory */
    uint32_t ReadPixel(uint32_t addr, int bpp) const;
    void WritePixel(uint32_t addr, uint32_t val, int bpp);
    void MarkDirty(uint32_t addr, uint32_t len);

    /* the 2D engine */
    bool GatherOp(BltOp *op);
    void Run2D(bool from_launch, uint32_t launch);
    void LaunchWrite(uint32_t val);
    void DrawPixel(const BltOp &op, int x, int y, uint32_t src, bool src_valid);
    uint32_t PatternPixel(const BltOp &op, int x, int y, bool *opaque) const;
    void ScreenBlt(const BltOp &op);
    void RectFill(const BltOp &op);
    void DrawLine(const BltOp &op, bool skip_last);
    void HostBltStart(const BltOp &op);
    void HostBltData(uint32_t val);
    bool HostBltSpan();

    /* the display */
    void ApplyMode();
    void DrawRows(int y0, int y1);
    bool UpdatePalette();
    void RefreshCursor(HostScreen *screen);

public:
    BansheeDevice(DeviceContext *ctx, const char *name, uint16_t device_id,
                  int vram_mb, int width, int height);
    ~BansheeDevice() override;

    bool Prepare() override;
    bool Realize() override;

    /* FBDevice */
    void Refresh(HostScreen *screen) override;

    /* PCIBarTarget */
    void SetBar(int bar_num, uint64_t addr, bool enabled) override;

    DeviceIOAdapter<BansheeDevice, &BansheeDevice::RegRead,
                    &BansheeDevice::RegWrite> fRegIo {*this};
    DeviceIOAdapter<BansheeDevice, &BansheeDevice::PortRead,
                    &BansheeDevice::PortWrite> fPortIo {*this};
};
