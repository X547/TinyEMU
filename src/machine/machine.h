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

#include <stdint.h>

#include "json.h"

/* This header is included both by the machines, which have already pulled in
   iomem.h/virtio.h, and by the bus and resource code, which has not. */
class BlockDevice;
class CharacterDevice;
class EthernetDevice;
class FSDevice;
class PhysMemoryMap;
class SerialState;
class StartCallback;
struct PCIBus;
struct PhysMemoryRange;
struct VIRTIODevice;

class FBDevice;

/* Implemented by the display back end; called for each dirty rectangle. */
class SimpleFBDraw {
public:
    virtual ~SimpleFBDraw() = default;

    virtual void Draw(FBDevice *fb_dev, int x, int y, int w, int h) = 0;
};


/* A device that takes host input events. Which device model holds the
   keyboard and the pointer is not the front end's business. */
class InputEventTarget {
public:
    virtual ~InputEventTarget() = default;

    /* 'key_code' is a Linux evdev key code, which is what the front ends
       produce. */
    virtual void SendKeyEvent(bool is_down, uint16_t key_code)
        {(void)is_down; (void)key_code;}

    /* When MouseIsAbsolute(), dx and dy are a position in 0..32767 rather
       than a displacement. */
    virtual void SendMouseEvent(int dx, int dy, int dz, unsigned int buttons)
        {(void)dx; (void)dy; (void)dz; (void)buttons;}

    virtual bool MouseIsAbsolute() {return false;}
};


/* Implemented by a device that answers the VMware backdoor port. The call
   carries the processor's registers rather than the port's data, which is how
   the protocol passes its arguments, so the machine performs the access and
   the device only interprets it. */
class VMPortTarget {
public:
    virtual ~VMPortTarget() = default;

    virtual void VMPortCommand(uint32_t *regs) = 0;
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

#define VM_CONFIG_VERSION 2

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

typedef struct VMDeviceNode VMDeviceNode;
typedef struct VirtMachineClass VirtMachineClass;

/* One node of the configuration's device tree. A node that declares child
   devices provides a bus; the nesting in the config file is the nesting of
   the buses in the machine.

   'props' points into the parsed configuration, which VirtMachineParams keeps
   alive for as long as the tree exists. The back end pointers are filled in
   between parsing and machine construction, by whoever can open files and
   sockets. */
struct VMDeviceNode {
    char *type;
    char *id;
    JSONValue props;

    VMDeviceNode *parent;
    VMDeviceNode *next; /* next sibling */
    VMDeviceNode *children;
    int child_count;
    /* The bus type the configuration declared for those children, checked
       against the bus the device actually provides. */
    char *child_bus_type;

    /* resolved back ends */
    char *filename;
    BlockDevice *block_dev;
    FSDevice *fs_dev;
    EthernetDevice *net;
};

typedef struct {
    char *cfg_filename;
    const VirtMachineClass *vmc;
    char *machine_name;
    uint64_t ram_size;
    int cpu_count;
    bool rtc_real_time;
    bool rtc_local_time;
    /* Whether the configuration declares a display, and how big. The window
       is opened before the machine exists, so this much is read out of the
       tree ahead of time; everything else about the device comes from the
       tree itself. NULL means no display. */
    char *display_device;
    int width, height; /* graphic width & height */
    CharacterDevice *console;

    char *cmdline; /* bios or kernel command line */
    bool accel_enable; /* enable acceleration (KVM) */

    /* The device tree the configuration declares, and the type its root bus
       was given. Every machine builds from these. */
    VMDeviceNode *root_devices;
    char *root_bus_type;
    /* the parsed configuration, kept alive because the nodes point into it */
    JSONValue cfg_json;

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
    /* 16550 console, when the machine has one. Console input goes here only
       if no virtio console is present to take it. */
    SerialState *serial_console = nullptr;

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
int vm_get_str(JSONValue obj, const char *name, const char **pstr);
int vm_get_str_opt(JSONValue obj, const char *name, const char **pstr);

/* Depth first walk over the configuration's device tree. */
typedef void (*VMDeviceNodeVisitor)(VMDeviceNode *node, void *opaque);
void vm_walk_devices(VMDeviceNode *node, VMDeviceNodeVisitor visit,
                     void *opaque);

void virt_machine_set_defaults(VirtMachineParams *p);
void virt_machine_load_config_file(VirtMachineParams *p,
                                   const char *filename,
                                   StartCallback *start);
void vm_add_cmdline(VirtMachineParams *p, const char *cmdline);
char *get_file_path(const char *base_filename, const char *filename);
void virt_machine_free_config(VirtMachineParams *p);
VirtMachine *virt_machine_init(VirtMachineParams *p);

/* gui */
void sdl_refresh(VirtMachine *m);
void sdl_init(int width, int height);

/* block_net.c */
BlockDevice *block_device_init_http(const char *url, int max_cache_size_kb,
                                    StartCallback *start);
