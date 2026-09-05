/*
 * VM definitions
 * 
 * Copyright (c) 2016-2017 Fabrice Bellard
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

#include "json.h"

class FBDevice;

/* Implemented by the display back end; called for each dirty rectangle. */
class SimpleFBDraw {
public:
    virtual ~SimpleFBDraw() = default;

    virtual void Draw(FBDevice *fb_dev, int x, int y, int w, int h) = 0;
};


class FBDevice {
public:
    /* the following is set by the device */
    int width = 0;
    int height = 0;
    int stride = 0; /* current stride in bytes */
    uint8_t *fb_data = nullptr; /* current pointer to the pixel data */
    int fb_size = 0; /* frame buffer memory size (info only) */

    virtual ~FBDevice() = default;

    virtual void Refresh(SimpleFBDraw *draw) = 0;
};

#define MAX_DRIVE_DEVICE 4
#define MAX_FS_DEVICE 4
#define MAX_ETH_DEVICE 1

#define VM_CONFIG_VERSION 1

typedef enum {
    VM_FILE_BIOS,
    VM_FILE_VGA_BIOS,
    VM_FILE_KERNEL,
    VM_FILE_INITRD,

    VM_FILE_COUNT,
} VMFileTypeEnum;

typedef struct {
    char *filename;
    uint8_t *buf;
    int len;
} VMFileEntry;

typedef struct {
    char *device;
    char *filename;
    BlockDevice *block_dev;
} VMDriveEntry;

typedef struct {
    char *device;
    char *tag; /* 9p mount tag */
    char *filename;
    FSDevice *fs_dev;
} VMFSEntry;

typedef struct {
    char *driver;
    char *ifname;
    EthernetDevice *net;
} VMEthEntry;

typedef struct VirtMachineClass VirtMachineClass;

typedef struct {
    char *cfg_filename;
    const VirtMachineClass *vmc;
    char *machine_name;
    uint64_t ram_size;
    bool rtc_real_time;
    bool rtc_local_time;
    char *display_device; /* NULL means no display */
    int width, height; /* graphic width & height */
    CharacterDevice *console;
    VMDriveEntry tab_drive[MAX_DRIVE_DEVICE];
    int drive_count;
    VMFSEntry tab_fs[MAX_FS_DEVICE];
    int fs_count;
    VMEthEntry tab_eth[MAX_ETH_DEVICE];
    int eth_count;

    char *cmdline; /* bios or kernel command line */
    bool accel_enable; /* enable acceleration (KVM) */
    char *input_device; /* NULL means no input */
    
    /* kernel, bios and other auxiliary files */
    VMFileEntry files[VM_FILE_COUNT];
} VirtMachineParams;

class VirtMachine {
public:
    const VirtMachineClass *vmc = nullptr;
    /* network */
    EthernetDevice *net = nullptr;
    /* console */
    VIRTIODevice *console_dev = nullptr;
    CharacterDevice *console = nullptr;
    /* graphics */
    FBDevice *fb_dev = nullptr;

    virtual ~VirtMachine() = default;

    /* in ms */
    virtual int GetSleepDuration(int delay) = 0;
    virtual void Interp(int max_exec_cycle) = 0;
    virtual bool MouseIsAbsolute() = 0;
    virtual void SendMouseEvent(int dx, int dy, int dz,
                                unsigned int buttons) = 0;
    virtual void SendKeyEvent(bool is_down, uint16_t key_code) = 0;
};


/* The parts of a machine type that exist before any instance does: its name
   and its factory. */
class VirtMachineClass {
public:
    virtual ~VirtMachineClass() = default;

    virtual const char *MachineNames() const = 0;
    virtual void SetDefaults(VirtMachineParams *p) const = 0;
    virtual VirtMachine *Init(const VirtMachineParams *p) const = 0;
};

extern const VirtMachineClass &gRiscvMachineClass;
extern const VirtMachineClass &gPcMachineClass;

void __attribute__((format(printf, 1, 2))) vm_error(const char *fmt, ...);
int vm_get_int(JSONValue obj, const char *name, int *pval);
int vm_get_int_opt(JSONValue obj, const char *name, int *pval, int def_val);

void virt_machine_set_defaults(VirtMachineParams *p);
void virt_machine_load_config_file(VirtMachineParams *p,
                                   const char *filename,
                                   StartCallback *start);
void vm_add_cmdline(VirtMachineParams *p, const char *cmdline);
char *get_file_path(const char *base_filename, const char *filename);
void virt_machine_free_config(VirtMachineParams *p);
VirtMachine *virt_machine_init(const VirtMachineParams *p);

/* gui */
void sdl_refresh(VirtMachine *m);
void sdl_init(int width, int height);

/* simplefb.c */
class SimpleFBState;
FBDevice *simplefb_init(PhysMemoryMap *map, uint64_t phys_addr,
                        int width, int height);
void simplefb_refresh(FBDevice *fb_dev, SimpleFBDraw *draw,
                      PhysMemoryRange *mem_range, int fb_page_count);

/* vga.c */
class VGAState;
FBDevice *pci_vga_init(PCIBus *bus, int width, int height,
                       const uint8_t *vga_rom_buf, int vga_rom_size);
                      
/* block_net.c */
BlockDevice *block_device_init_http(const char *url, int max_cache_size_kb,
                                    StartCallback *start);
