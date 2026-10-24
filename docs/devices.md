# TinyEMU virtual devices

This document lists every device type a TinyEMU machine configuration can
declare, the properties each one takes, and an example definition for each.
For a whole machine, see the commented samples
[`sample-riscv64.cfg`](../sample-riscv64.cfg) and
[`sample-pc.cfg`](../sample-pc.cfg); for background on the device models,
see section 3 of [`readme.txt`](../readme.txt).

- [1. The configuration file](#1-the-configuration-file)
- [2. Machine properties](#2-machine-properties)
- [3. Buses and device definitions](#3-buses-and-device-definitions)
- [4. Properties shared by several devices](#4-properties-shared-by-several-devices)
- [5. Device reference](#5-device-reference)
  - [Serial, console and power](#serial-console-and-power)
  - [PCI host bridges and switches](#pci-host-bridges-and-switches)
  - [PC platform devices](#pc-platform-devices)
  - [Display](#display)
  - [Input](#input)
  - [Storage](#storage)
  - [Network](#network)
  - [Audio](#audio)
  - [Filesystem](#filesystem)
- [6. Retired device types](#6-retired-device-types)

## 1. The configuration file

A configuration is a JSON file with a relaxed syntax: keys need no quotes,
trailing commas are accepted, and `/* ... */` comments may appear anywhere.
The current format is version 2, which declares every device inside the bus
it attaches to. The emulator rejects any other version.

```js
{
    version: 2,
    machine: "riscv64",
    memory_size: 256,
    bios: "bbl64.bin",
    kernel: "kernel-riscv64.bin",
    cmdline: "console=hvc0 root=/dev/vda rw",
    bus: {
        type: "fdt",
        devices: [
            { type: "virtio-console" },
            { type: "virtio-block", file: "root-riscv64.bin" },
        ],
    },
}
```

File names (`bios`, `kernel`, a device's `file`, and so on) are resolved
relative to the configuration file.

## 2. Machine properties

These keys sit at the top level of the file, next to `bus`.

| Key | Machines | Meaning |
| --- | --- | --- |
| `version` | all | Required. Must be `2`. |
| `machine` | all | Required. `"riscv32"`, `"riscv64"`, `"riscv128"` (if built with `-Dint128=true`) or `"pc"`. |
| `memory_size` | all | Required. RAM size in MB. The `-m` command line option overrides it. |
| `bus` | all | Required. The root bus and the devices on it; see [section 3](#3-buses-and-device-definitions). |
| `cpus` | all | Number of processors, default 1. Up to 64 harts on RISC-V. Up to 255 on the PC with `interrupt_controller: "apic"`, otherwise 1. |
| `interrupt_controller` | all | RISC-V: `"plic"` (default), `"aplic"` or `"aplic-imsic"`. PC: `"pic"` (default, the two 8259s only) or `"apic"` (adds a local APIC and an IOAPIC). |
| `bios` | all | Firmware image. On RISC-V it runs in M-mode. On the PC it is SeaBIOS or similar. |
| `kernel` | all | Kernel image. On the PC it is a bzImage, or an ELF with a PVH entry point, and the machine builds ACPI tables for it when the root bus is `acpi`. |
| `initrd` | all | Initial RAM disk. |
| `cmdline` | all | Kernel command line. `-append` adds to it. |
| `sbi` | RISC-V | `"firmware"` (default) runs `bios` in M-mode. `"native"` loads no bios, and the emulator itself answers SBI calls. |
| `compatible` | RISC-V | A string put before `"ucbbar,riscvemu-bar_dev"` in the device tree's root `compatible`, for a guest that recognises only the machine it names, such as `"riscv-virtio"` for QEMU virt. |
| `vga_bios` | PC | The option ROM of the `vga` device. |
| `accel` | all | `"auto"` (default) uses a host hypervisor when there is one. `"none"` always uses the interpreter, as `-no-accel` does. |
| `hypervisor_intc` | all | Boolean, default `true`. Lets the hypervisor provide the interrupt controllers it can. |
| `hypervisor_cpuid` | PC | `"host"` (default) or `"interpreter"`: which processor CPUID shows the guest under a hypervisor. |
| `hypervisor_pat` | PC | `"default"` or `"write-back"`: the IA32_PAT value the processors start with under a hypervisor. |
| `rtc_local_time` | PC | Boolean. The CMOS clock keeps local time instead of UTC, as Windows expects. |

The readme describes `interrupt_controller`, `sbi`, `accel` and the
`hypervisor_*` keys in detail.

## 3. Buses and device definitions

### Device objects

Each entry of a bus's `devices` array is an object:

```js
{
    type: "<device type>",   // required: one of the types in section 5
    id: "<name>",            // optional: the device's name in messages
                             // and in the device tree
    ...properties...,        // whatever that type takes
    bus: {                   // only for a device that provides a bus
        type: "<bus type>",  // must match the bus the device provides
        devices: [ ... ],
    },
}
```

An unknown `type`, an unknown quirk name, or a property of the wrong kind
(for example a string where a number is expected) is reported as an error,
and the machine does not start. Each bus holds at most 32 devices.

### Nesting

A device that provides a bus carries a nested `bus` object, and the `type`
written there is checked against the bus the device provides. Buses nest to
any depth. For example:

```
FDT bus -> pci-host-ecam-generic -> PCI bus -> xhci -> USB bus
        -> usb-storage -> SCSI bus -> scsi-disk
```

| Bus `type` | Provided by | Carries |
| --- | --- | --- |
| `fdt` | the RISC-V machine (root) | memory-mapped devices, PCI host bridges |
| `pc` | the PC machine (root), for firmware | `ns16550a`, `i8042`, `hpet`, `ne2000`, `pci-host-i440fx`, `pci-host-cloudhv` |
| `acpi` | the PC machine (root), for a kernel it loads | as `pc`, and `dw-i2c`; described in the DSDT |
| `pci` | PCI host bridges, `pci-bridge` | PCI devices |
| `ps2` | `i8042` | `ps2-keyboard`, `ps2-mouse` |
| `ata` | `pci-ide` | `ata-disk`, `atapi` |
| `scsi` | `atapi`, `buslogic`, `virtio-scsi`, `usb-storage` | `scsi-disk`, `scsi-cd` |
| `nvme` | `nvme` | `nvme-ns` |
| `sd` | `sdhci`, `dw-mmc` | `sd-card`, `mmc-card` |
| `usb` | `xhci`, `usb-hub` | `usb-hub`, `usb-storage`, `usb-hid`, `usb-audio` |
| `hid` | `usb-hid`, `i2c-hid` | `hid-keyboard`, `hid-tablet` |
| `i2c` | `dw-i2c` | `i2c-hid` |
| `mdio` | `dwmac` | `ethernet-phy` |
| `hda` | `intel-hda` | `hda-codec` |
| `hda-codec` | `hda-codec` | `hda-audio-group` |
| `hda-audio-group` | `hda-audio-group` | `hda-output` |
| `ac97` | `ich-ac97` | `ac97-codec` |
| `usb-audio` | `usb-audio` | `usb-audio-output` |

### Addresses and interrupts

On an FDT (RISC-V) machine you never write MMIO addresses or interrupt lines.
The machine allocates them when it is built, checks them for overlaps, and
describes what it assigned in the device tree. The only exception is
`cfi-flash`, which takes an optional `base`.

The PC machine places nothing. Its devices sit at the fixed addresses a PC
has always used, and the machine only checks that two devices do not
overlap. Only `ns16550a`, `i8042`, `hpet` and the ISA form of `ne2000` go
straight on the root bus. Everything else is a PCI device and goes inside
`pci-host-i440fx` or `pci-host-cloudhv`.

The PC's root bus is `pc` when firmware (`bios`) runs, since firmware builds
its own ACPI tables. It is `acpi` when the machine loads `kernel` itself: the
machine then builds the tables, and describes the devices on the root bus in
the DSDT along with the 8259s, the PIT and the RTC. A kernel finds other
processors and the IOAPIC only in those tables, so `cpus` above 1 or
`interrupt_controller: "apic"` with a `kernel` need `acpi`.

A device on the PC that names no interrupt line, such as `dw-i2c` or
`i2c-hid`, gets one of the IOAPIC's inputs 16-23 with
`interrupt_controller: "apic"`, and a free ISA line otherwise.

### PCI Express topology

On the ECAM bridge, bus 0 holds as many devices as it has slots. Behind the
root port of a DesignWare or PLDA bridge, and behind each downstream port of
a `pci-bridge`, the link carries exactly one device. To place several devices
there, nest them inside a `pci-bridge`.

## 4. Properties shared by several devices

### Disk images: `file`, `read_only`

Block devices read `file`, an image path relative to the configuration file.
It can also be the URL of a `blk.txt` produced by `splitimg`, which makes it
an HTTP block device. Unless `-rw` is given, writes go to an in-memory
snapshot and the image file is left unchanged. `read_only: 1` additionally
tells the guest that the medium is write-protected.

```js
{ type: "virtio-block", file: "disk.img", read_only: 1 }
```

### Network back end: `driver`, `ifname`, `forward`

Network adapters (`virtio-net`, `ne2000`, `eepro100`, `dwmac`) read:

| Property | Meaning |
| --- | --- |
| `driver` | Required. `"user"` (built-in NAT with DHCP, through slirp) or `"tap"`. |
| `ifname` | The tap interface name, for `"tap"`. |
| `forward` | `"user"` only: an array of host ports to forward to the guest. |

Each `forward` entry takes `host_port` and `guest_port` (both required,
1-65535), `proto` (`"tcp"` default, or `"udp"`), `host_addr` (default
`"127.0.0.1"`; use `"0.0.0.0"` to accept connections from the whole network)
and `guest_addr` (default: the address the built-in DHCP server hands out).

```js
{
    type: "virtio-net",
    driver: "user",
    forward: [
        { host_port: 2222, guest_port: 22 },
        { proto: "udp", host_port: 5353, guest_port: 53 },
    ],
}
```

Only one network device may be declared per machine.

### Audio back end: `host`

Audio outputs (`hda-output`, `usb-audio-output`, `ac97-codec`) read a `host`
object that says where the sound goes. Without one, the host's audio system
is used.

| Key | Meaning |
| --- | --- |
| `driver` | `"host"` (default): WASAPI on Windows, PipeWire on Linux, the Media Kit on Haiku; on a host without an audio back end it behaves as `"none"`. `"wav"` writes to a WAV file. `"none"` produces no sound, but the guest's streams still play in real time. |
| `device` | `"host"` on Windows and Linux: part of the output device's name. Without it, the default output is used, and followed when the user changes it. |
| `file` | `"wav"`: the WAV file to write, or for a capture input the file to read. |
| `loop` | `"wav"` capture only: `1` restarts the file when it ends. |
| `latency` | How far ahead of the listener, in ms, the guest may run. 1-1000, default 20. |

```js
host: { driver: "wav", file: "guest-audio.wav" }
```

Host back ends cannot record yet, so an input that captures from `"host"`
hears silence. Use `"wav"` to feed the guest a file.

### Device workarounds: `quirks`

`nvme` and `dwmac` read `quirks`, an array of names that switches on
non-standard behaviour some guests need. An unknown name is an error.
Without `quirks`, the device follows its specification.

## 5. Device reference

Placement is given as the bus the device goes on: **FDT** is the RISC-V root
bus, **PC** is the PC root bus (`pc` or `acpi`), and **PCI** is any PCI bus (behind any host
bridge, on either machine).

The examples are fragments: each is one entry of the `devices` array of the
bus named in its comment.

### Serial, console and power

#### `ns16550a` — 16550A UART

Placement: FDT or PC.

| Property | Default | Meaning |
| --- | --- | --- |
| `reg` | `0x3f8` | PC only: the I/O port base. |
| `irq` | `4` | PC only: the ISA interrupt line. |

On an FDT machine its registers and interrupt are allocated, and it becomes
`/chosen/stdout-path`. On the PC it is COM1 unless `reg` and `irq` say
otherwise.

```js
/* fdt or pc bus */
{ type: "ns16550a" }

/* pc bus: COM2 */
{ type: "ns16550a", reg: 0x2f8, irq: 3 }
```

#### `virtio-console` — virtio console

Placement: FDT (virtio-mmio) or PCI. No properties. Connects the guest to the
emulator's console (`hvc0` in Linux).

```js
{ type: "virtio-console" }
```

#### `syscon-poweroff` — power-off register

Placement: FDT. No properties. A SiFive-test-compatible register, described as
a `syscon` and `syscon-poweroff` pair, through which OpenSBI and Linux power
off. Writing `0x5555` exits the emulator with status 0, and
`(code << 16) | 0x3333` exits with `code`.

```js
{ type: "syscon-poweroff" }
```

The RISC-V machine also always has an HTIF console and power-off register at
`0x40008000`, described in the device tree as `ucb,htif0` with `fromhost` and
`tohost` in that order. It is not declared in the configuration.

### PCI host bridges and switches

#### `pci-host-ecam-generic` — generic ECAM PCIe host bridge

Placement: FDT. Provides a `pci` bus. Devices sit directly on bus 0 and
interrupt over INTx (MSI-X is available with `interrupt_controller:
"aplic-imsic"`).

| Property | Default | Meaning |
| --- | --- | --- |
| `bus_count` | `16` | ECAM window size in MB (one bus per MB). |
| `mmio_size` | `256` | 32-bit memory aperture, in MB. |
| `mmio64_size` | `4096` | Memory aperture above 4 GB, in MB; `0` for none. |
| `io_size` | `64` | I/O aperture in KB, a power of two of at least 4; `0` for none. |

```js
/* fdt bus */
{
    type: "pci-host-ecam-generic",
    id: "pcie0",
    mmio_size: 256,
    bus: {
        type: "pci",
        devices: [
            { type: "virtio-net", driver: "user" },
            { type: "virtio-block", file: "root.img" },
        ],
    },
}
```

#### `pci-host-designware` — Synopsys DesignWare PCIe root complex

Placement: FDT. Provides a `pci` bus behind its own root port, which carries
one device. It has an MSI receiver, so devices on it offer MSI-X and MSI.

| Property | Default | Meaning |
| --- | --- | --- |
| `mmio_size`, `mmio64_size`, `io_size` | as ECAM | Aperture sizes. |
| `bus_count` | `16` | Bus range advertised in the device tree. |
| `compatible` | `"sifive,fu740-pcie"` | Which controller the device tree node claims to be. Use `"snps,dw-pcie"` for Linux's generic driver. |

```js
/* fdt bus */
{
    type: "pci-host-designware",
    id: "pcie1",
    compatible: "snps,dw-pcie",
    bus: {
        type: "pci",
        devices: [
            { type: "nvme", bus: { type: "nvme", devices: [
                { type: "nvme-ns", file: "nvme.img" },
            ] } },
        ],
    },
}
```

#### `pci-host-plda` — PLDA XpressRich PCIe root complex

Placement: FDT. The StarFive JH7110's bridge (`starfive,jh7110-pcie`).
Provides a `pci` bus behind a root port that carries one device, and has an
MSI receiver. It has no I/O aperture, and it brings the syscon and reset
controller nodes its driver requires. Declare it among the first two host
bridges, because a guest may accept only PCI domains 0 and 1.

| Property | Default | Meaning |
| --- | --- | --- |
| `mmio_size`, `mmio64_size`, `bus_count` | as ECAM | Aperture sizes and bus range. |

```js
/* fdt bus */
{
    type: "pci-host-plda",
    id: "pcie2",
    bus: { type: "pci", devices: [ { type: "virtio-net", driver: "user" } ] },
}
```

#### `pci-host-i440fx` — i440FX/PIIX3 PC host bridge

Placement: PC. No properties. Provides a `pci` bus, the CF8/CFC
configuration ports, and the PIIX3 ISA bridge that routes INTx onto the
8259s. Every PCI device of a PC goes inside it. PIRQA-D take lines 10 and
11, which no other device may then use.

```js
/* pc bus */
{
    type: "pci-host-i440fx",
    bus: {
        type: "pci",
        devices: [
            { type: "vga", width: 1024, height: 768 },
            { type: "virtio-block", file: "disk.img" },
        ],
    },
}
```

#### `pci-host-cloudhv` — Cloud Hypervisor's PC host bridge

Placement: PC. No properties. Like `pci-host-i440fx`, but the host bridge
function is 8086:0d57 and the aperture below 4 GB is fixed at
`0xc0000000-0xf7ffffff`, as EDK2's Cloud Hypervisor build expects.

```js
/* pc bus */
{ type: "pci-host-cloudhv", bus: { type: "pci", devices: [
    { type: "virtio-block", file: "disk.img" },
] } }
```

#### `pci-bridge` — PCI Express switch

Placement: PCI. No properties. An upstream port with a downstream port for
each device nested inside it. Use it to put several devices behind a root
port, or nest switches to build a deeper hierarchy.

```js
/* pci bus behind a pci-host-designware */
{
    type: "pci-bridge",
    id: "sw0",
    bus: {
        type: "pci",
        devices: [
            { type: "virtio-net", driver: "user" },
            { type: "virtio-input", kind: "keyboard" },
        ],
    },
}
```

### PC platform devices

The PC machine always has RAM, the two 8259 PICs, the 8254 PIT and the
MC146818 RTC/CMOS. These are not declared. On the `acpi` root bus the
machine describes them in the DSDT, and line 9 is the SCI's.

#### `i8042` — PC keyboard controller

Placement: PC. No properties. Provides a `ps2` bus with two ports.

```js
/* pc bus */
{
    type: "i8042",
    bus: {
        type: "ps2",
        devices: [ { type: "ps2-keyboard" }, { type: "ps2-mouse" } ],
    },
}
```

#### `hpet` — High Precision Event Timer

Placement: PC. No properties. A 100 MHz counter with three timers at
`0xfed00000`, described in the ACPI tables.

```js
/* pc bus */
{ type: "hpet" }
```

### Display

There is one emulator window, and the display device set up last is the one
it shows. Declare a single display device.

#### `simplefb` — simple frame buffer

Placement: FDT. A linear frame buffer, described by a `simple-framebuffer`
device tree node.

| Property | Default | Meaning |
| --- | --- | --- |
| `width` | required | Width in pixels. |
| `height` | required | Height in pixels. |

```js
/* fdt bus */
{ type: "simplefb", width: 1024, height: 768 }
```

#### `vga` — standard VGA with VBE

Placement: PCI. Also decodes the legacy VGA and VBE ports. Its ROM is the
machine's `vga_bios`. It displays text modes and the VBE linear modes at 8,
15, 16, 24 and 32 bpp. Planar and chained modes in the `0xa0000` window are
not drawn.

Like QEMU's stdvga, it has a register BAR next to the frame buffer: an EDID
whose preferred timing is the largest mode, the VGA ports and the VBE
registers. EDK2's `QemuVideoDxe` starts in that mode, and firmware and
drivers reach the registers there rather than through the ports.

| Property | Default | Meaning |
| --- | --- | --- |
| `width` | required | Width of the largest mode, and the window's initial width. |
| `height` | required | Height of the largest mode, and the window's initial height. |

```js
/* pci bus */
{ type: "vga", width: 1280, height: 1024 }
```

#### `banshee` — 3dfx Voodoo Banshee / Voodoo3

Placement: PCI. A 2D drawing engine and a hardware cursor, with no VGA core,
so the screen stays blank until a guest driver sets a mode.

| Property | Default | Meaning |
| --- | --- | --- |
| `vram` | `16` | Video memory in MB, a power of two from 4 to 32. |
| `model` | `"banshee"` | `"banshee"` or `"voodoo3"`. Only changes the IDs a driver matches on. |
| `width`, `height` | `1024`, `768` | The window's initial size. |

```js
/* pci bus */
{ type: "banshee", model: "voodoo3", vram: 16 }
```

#### `virtio-gpu` — virtio 2D GPU

Placement: FDT or PCI. A 2D display with a hardware cursor. The guest is
asked to follow when the window is resized.

| Property | Default | Meaning |
| --- | --- | --- |
| `width`, `height` | `1024`, `768` | Size first offered to the guest, 64-8192. |

```js
{ type: "virtio-gpu", width: 1280, height: 800 }
```

### Input

The emulator window sends keyboard and pointer events to the last keyboard
and the last pointer that were set up. Declare either `virtio-input`, PS/2 or
HID devices for each role, not several of them.

#### `virtio-input` — virtio input device

Placement: FDT or PCI.

| Property | Default | Meaning |
| --- | --- | --- |
| `kind` | required | `"keyboard"`, `"mouse"` (relative) or `"tablet"` (absolute). |

```js
{ type: "virtio-input", kind: "keyboard" },
{ type: "virtio-input", kind: "tablet" }
```

#### `ps2-keyboard`, `ps2-mouse` — PS/2 devices

Placement: the `ps2` bus of an `i8042`.

| Property | Default | Meaning |
| --- | --- | --- |
| `port` | first free | `0` (keyboard port) or `1` (aux port). |

```js
/* ps2 bus: mouse first, so it sits on port 0 */
{ type: "ps2-mouse" },
{ type: "ps2-keyboard" }

/* or explicitly */
{ type: "ps2-keyboard", port: 0 },
{ type: "ps2-mouse", port: 1 }
```

#### `hid-keyboard`, `hid-tablet` — HID functions

Placement: the `hid` bus of a `usb-hid` or an `i2c-hid`. Either transport
carries the same functions.

| Property | Default | Meaning |
| --- | --- | --- |
| `index` | first free | Which function of the transport to be (up to 4 on `usb-hid`, 1 on `i2c-hid`). |

```js
/* hid bus */
{ type: "hid-keyboard" },
{ type: "hid-tablet" }
```

#### `usb-hid` — USB HID device

Placement: `usb` bus. Provides a `hid` bus with up to four functions, each on
its own interface. A guest whose driver binds one function per USB device
needs a separate `usb-hid` for each function.

| Property | Default | Meaning |
| --- | --- | --- |
| `port` | first free | The parent's port to occupy. |

```js
/* usb bus */
{
    type: "usb-hid",
    bus: {
        type: "hid",
        devices: [ { type: "hid-keyboard" }, { type: "hid-tablet" } ],
    },
}
```

#### `i2c-hid` — HID over I2C

Placement: `i2c` bus. Provides a `hid` bus carrying one function, and takes
an interrupt line of its own. In ACPI it is a `PNP0C50` device whose `_DSM`
gives the HID descriptor's register.

| Property | Default | Meaning |
| --- | --- | --- |
| `reg` | first free from `0x08` | 7-bit address, `0x08`-`0x77`. |

```js
/* i2c bus */
{
    type: "i2c-hid",
    reg: 0x2c,
    bus: { type: "hid", devices: [ { type: "hid-keyboard" } ] },
}
```

#### `dw-i2c` — Synopsys DesignWare I2C controller

Placement: FDT, or the PC's `acpi` root bus. Provides an `i2c` bus. The bus
places targets that name no address and describes them in the device tree
or the DSDT. In ACPI the controller carries `SSCN` and `FMCN`, so a driver
needs no reference clock.

| Property | Default | Meaning |
| --- | --- | --- |
| `compatible` | `"snps,designware-i2c"` | Device tree compatible string. |
| `acpi_hid` | `"AMDI0010"` | The `_HID` in ACPI. |

```js
/* fdt or acpi bus */
{
    type: "dw-i2c",
    id: "i2c0",
    bus: {
        type: "i2c",
        devices: [
            { type: "i2c-hid", bus: { type: "hid", devices: [
                { type: "hid-keyboard" },
            ] } },
            { type: "i2c-hid", bus: { type: "hid", devices: [
                { type: "hid-tablet" },
            ] } },
        ],
    },
}
```

### Storage

#### `virtio-block` — virtio block device

Placement: FDT or PCI.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Disk image or `blk.txt` URL. |
| `read_only` | `0` | Report the disk as write-protected. |

```js
{ type: "virtio-block", file: "root-riscv64.bin" }
```

#### `virtio-scsi` — virtio SCSI host

Placement: FDT or PCI. No properties. Provides a `scsi` bus.

```js
{
    type: "virtio-scsi",
    bus: {
        type: "scsi",
        devices: [
            { type: "scsi-disk", file: "disk.img" },
            { type: "scsi-cd", file: "install.iso" },
        ],
    },
}
```

#### `pci-ide` — PCI IDE controller

Placement: PCI. Provides an `ata` bus and needs at least one drive on it.
Bus-master DMA is supported. On the PC it runs in compatibility mode
(`0x1f0`/`0x170`, IRQ 14/15). Elsewhere it runs in native mode, with all
registers in BARs and INTx interrupts. Drives fill the master and slave of
channel 0, then of channel 1, in the order they are listed.

```js
/* pci bus */
{
    type: "pci-ide",
    bus: {
        type: "ata",
        devices: [
            { type: "ata-disk", file: "disk.img" },   // primary master
            { type: "atapi", bus: { type: "scsi", devices: [  // primary slave
                { type: "scsi-cd", file: "cd.iso" },
            ] } },
        ],
    },
}
```

#### `ata-disk` — ATA hard disk

Placement: `ata` bus.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Disk image. |
| `read_only` | `0` | Report the disk as write-protected. |

```js
/* ata bus */
{ type: "ata-disk", file: "disk.img" }
```

#### `atapi` — ATAPI packet device

Placement: `ata` bus. No properties. Provides a `scsi` bus carrying one
device (usually a `scsi-cd`), whose commands it passes over ATA.

```js
/* ata bus */
{ type: "atapi", bus: { type: "scsi", devices: [
    { type: "scsi-cd", file: "cd.iso" },
] } }
```

#### `buslogic` — BusLogic BT-958 SCSI host adapter

Placement: PCI. No properties. Provides a `scsi` bus with targets 0-15 (7 is
the adapter itself) and LUNs 0-7, and needs at least one device on it. It has
no BIOS, so the guest cannot boot from it.

```js
/* pci bus */
{ type: "buslogic", bus: { type: "scsi", devices: [
    { type: "scsi-disk", file: "disk.img", target: 0 },
] } }
```

#### `scsi-disk` — SCSI disk

Placement: `scsi` bus.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Disk image. |
| `target` | first free | SCSI target ID. |
| `lun` | first free | Logical unit number. |
| `read_only` | `0` | Report the unit as write-protected. |

```js
/* scsi bus */
{ type: "scsi-disk", file: "data.img", lun: 1 }
```

#### `scsi-cd` — SCSI CD/DVD-ROM drive

Placement: `scsi` bus. An image larger than 80 minutes of blocks is
reported as a DVD. The guest can eject the disc and load it again.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | none (empty drive) | A plain ISO image (2048-byte blocks), opened read-only. |
| `target`, `lun` | first free | As for `scsi-disk`. |

```js
/* scsi bus */
{ type: "scsi-cd", file: "install.iso" }
```

#### `nvme` — NVM Express controller

Placement: PCI. Provides an `nvme` bus and needs at least one namespace.
It uses 64-bit BARs.

| Property | Default | Meaning |
| --- | --- | --- |
| `quirks` | none | Workarounds for non-conforming guests; see below. |

| Quirk | Effect |
| --- | --- |
| `no-enable-check` | Serve the admin queue as soon as AQA, ASQ and ACQ are programmed, without waiting for CC.EN. |
| `nsid-zero` | Treat namespace ID 0 as namespace 1. |
| `loose-queue-create` | Accept a queue creation that asks for a discontiguous queue or names completion queue 0, and clamp a read that runs past the end of the namespace instead of failing it. |
| `poll-only` | Never drive the INTx pin; the guest has to poll. |
| `haiku` | All four, which Haiku's RISC-V boot loader and disk driver need between them. |

```js
/* pci bus */
{
    type: "nvme",
    bus: {
        type: "nvme",
        devices: [
            { type: "nvme-ns", file: "nvme0.img" },
            { type: "nvme-ns", file: "nvme1.img", nsid: 2 },
        ],
    },
}
```

#### `nvme-ns` — NVMe namespace

Placement: `nvme` bus, up to 8 per controller.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Disk image. |
| `nsid` | first free | Namespace ID. |
| `read_only` | `0` | Report the namespace as write-protected. |

```js
/* nvme bus */
{ type: "nvme-ns", file: "nvme0.img" }
```

#### `sdhci` — SD host controller

Placement: PCI or FDT. Provides an `sd` bus with one slot. It supports PIO,
SDMA and ADMA2 transfers, and uses MSI-X or MSI where the bus has a receiver.

| Property | Default | Meaning |
| --- | --- | --- |
| `clock` | `50` | Base clock in MHz. |
| `compatible` | `"arasan,sdhci-8.9a"` | FDT only: device tree compatible string. |

```js
/* pci bus */
{ type: "sdhci", bus: { type: "sd", devices: [
    { type: "sd-card", file: "sd.img" },
] } }

/* fdt bus */
{ type: "sdhci", id: "mmc0", bus: { type: "sd", devices: [
    { type: "mmc-card", file: "emmc.img" },
] } }
```

#### `dw-mmc` — Synopsys DesignWare MMC host

Placement: FDT. Provides an `sd` bus with one slot.

| Property | Default | Meaning |
| --- | --- | --- |
| `clock` | `50` | Card interface clock in MHz. |
| `compatible` | `"snps,dw-mshc"` | Use `"starfive,jh7110-mmc"` for the JH7110 driver. |
| `dma` | `32` | Internal DMA descriptor address width, `32` or `64`; `0` for a FIFO-only controller. |

```js
/* fdt bus */
{
    type: "dw-mmc",
    id: "mmc1",
    compatible: "starfive,jh7110-mmc",
    bus: { type: "sd", devices: [ { type: "sd-card", file: "sd.img" } ] },
}
```

#### `sd-card`, `mmc-card` — SD card and eMMC device

Placement: `sd` bus. `sd-card` speaks the SD protocol and `mmc-card` the MMC
protocol. Blocks are 512 bytes. An image larger than 2 GB is reported as a
high-capacity card.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Card image. |
| `read_only` | `0` | Report the card as write-protected. |

```js
/* sd bus */
{ type: "sd-card", file: "sd.img" }

/* sd bus */
{ type: "mmc-card", file: "emmc.img" }
```

#### `cfi-flash` — CFI parallel NOR flash

Placement: FDT. A 32-bit bank of two 16-bit Intel command-set chips, as on
QEMU's virt machines. The guest reads it in place. What the guest programs is
saved back to the image in the same way as disk writes (so not without
`-rw`). With `sbi: "native"` and no `kernel`, the first flash is where
execution starts.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Contents; rounded up to a power of two with erased blocks. |
| `base` | allocated | Physical address, a multiple of 4 KB. |
| `block_size` | `256` | Erase block in KB, a power of two from 4 to 32768. |
| `read_only` | `0` | Refuse programming and erasing. |

```js
/* fdt bus: EDK2 code and variable store */
{ type: "cfi-flash", file: "RISCV_VIRT_CODE.fd", base: 0x20000000 },
{ type: "cfi-flash", file: "RISCV_VIRT_VARS.fd", base: 0x22000000 }
```

#### `xhci` — USB 3.0 host controller

Placement: PCI. Provides a `usb` bus. The device models are full or high
speed, so they use the USB 2.0 ports.

| Property | Default | Meaning |
| --- | --- | --- |
| `usb2_ports` | `4` | Number of USB 2.0 root ports. |
| `usb3_ports` | `2` | Number of USB 3.0 root ports. |

```js
/* pci bus */
{
    type: "xhci",
    bus: {
        type: "usb",
        devices: [
            { type: "usb-hid", bus: { type: "hid", devices: [
                { type: "hid-keyboard" },
            ] } },
        ],
    },
}
```

#### `usb-hub` — USB 2.0 hub

Placement: `usb` bus. Provides a `usb` bus.

| Property | Default | Meaning |
| --- | --- | --- |
| `ports` | `4` | Number of downstream ports. |
| `port` | first free | The parent's port to occupy. |

```js
/* usb bus */
{
    type: "usb-hub",
    ports: 4,
    bus: { type: "usb", devices: [
        { type: "usb-storage", bus: { type: "scsi", devices: [
            { type: "scsi-disk", file: "usb.img" },
        ] } },
    ] },
}
```

#### `usb-storage` — USB mass storage (bulk-only)

Placement: `usb` bus. Provides a `scsi` bus, and needs at least one device
on it.

| Property | Default | Meaning |
| --- | --- | --- |
| `port` | first free | The parent's port to occupy. |

```js
/* usb bus */
{
    type: "usb-storage",
    bus: {
        type: "scsi",
        devices: [
            { type: "scsi-disk", file: "usb-disk.img" },
            { type: "scsi-cd", file: "cd.iso" },
        ],
    },
}
```

### Network

Only one network device may be declared per machine. All of them take the
back-end properties in [section 4](#network-back-end-driver-ifname-forward).

#### `virtio-net` — virtio network adapter

Placement: FDT or PCI.

```js
{ type: "virtio-net", driver: "user" }

{ type: "virtio-net", driver: "tap", ifname: "tap0" }
```

#### `ne2000` — NE2000 Ethernet adapter

Placement: PCI (as an RTL8029AS) or PC (as an ISA card). It has no bus
master, so all data passes through its single data port.

| Property | Default | Meaning |
| --- | --- | --- |
| `reg` | `0x300` | PC bus only: I/O port base. |
| `irq` | `9` | PC bus only: ISA interrupt line. On the `acpi` root bus line 9 is the SCI's, so another has to be named. |

```js
/* pci bus */
{ type: "ne2000", driver: "user" }

/* pc bus */
{ type: "ne2000", driver: "user", reg: 0x320, irq: 10 }
```

#### `eepro100` — Intel EtherExpress PRO/100 (82558)

Placement: PCI. Includes its PHY and EEPROM. It moves frames by bus-master
DMA, so it is much faster than the `ne2000` under a hypervisor.

```js
/* pci bus */
{ type: "eepro100", driver: "user" }
```

#### `dwmac` — Synopsys DesignWare Ethernet QoS MAC

Placement: FDT. Provides an `mdio` bus and needs a PHY on it.

| Property | Default | Meaning |
| --- | --- | --- |
| `compatible` | `"snps,dwmac-5.10a"` | Must name a 4.x or 5.x core. `"snps,dwmac"` is always appended. |
| `phy_mode` | `"rgmii-id"` | `phy-mode` of the device tree node. |
| `quirks` | none | Workarounds for drivers written for a SoC that wraps this core; see below. |

| Quirk | Effect |
| --- | --- |
| `clocks` | Emit the clocks a StarFive-style platform binding names, as one fixed clock standing in for all of them. |
| `link-on-reset` | Report the link again after a software reset. |
| `haiku` | Both, which Haiku's driver needs together. |

```js
/* fdt bus */
{
    type: "dwmac",
    driver: "user",
    bus: { type: "mdio", devices: [ { type: "ethernet-phy", reg: 0 } ] },
}

/* fdt bus: what Haiku's driver needs */
{
    type: "dwmac",
    driver: "user",
    compatible: "starfive,jh7110-eqos-5.20",
    quirks: ["haiku"],
    bus: { type: "mdio", devices: [ { type: "ethernet-phy" } ] },
}
```

#### `ethernet-phy` — generic 10/100/1000 PHY

Placement: `mdio` bus.

| Property | Default | Meaning |
| --- | --- | --- |
| `reg` | first free | MDIO address. |
| `phy_id` | `0x00000041` | PHY identifier the guest reads. The default matches no vendor, so a generic driver binds to it. |

```js
/* mdio bus */
{ type: "ethernet-phy", reg: 1 }
```

### Audio

The stream plays at the pace of the host's audio clock, so no resampling is
done between the guest and the host.

#### `intel-hda` — Intel High Definition Audio controller (ICH6)

Placement: PCI. Provides an `hda` link.

| Property | Default | Meaning |
| --- | --- | --- |
| `input_streams` | `4` | Number of input stream engines. |
| `output_streams` | `4` | Number of output stream engines. |

```js
/* pci bus */
{
    type: "intel-hda",
    bus: {
        type: "hda",
        devices: [ {
            type: "hda-codec",
            bus: {
                type: "hda-codec",
                devices: [ {
                    type: "hda-audio-group",
                    bus: {
                        type: "hda-audio-group",
                        devices: [
                            { type: "hda-output", kind: "line-out" },
                            {
                                type: "hda-output",
                                kind: "headphone",
                                sequence: 1,
                                host: { driver: "wav", file: "phones.wav" },
                            },
                        ],
                    },
                } ],
            },
        } ],
    },
}
```

#### `hda-codec` — HDA codec

Placement: `hda` link, up to 15 codecs. Provides an `hda-codec` bus for its
function groups, and needs at least one group.

| Property | Default | Meaning |
| --- | --- | --- |
| `address` | first free | Codec address, 0-14. |
| `vendor_id` | `0x1af40010` | Vendor/device ID a driver matches on. |
| `subsystem_id` | `0` | Subsystem ID. |
| `revision_id` | `0x00100100` | Revision ID. |

```js
/* hda bus */
{ type: "hda-codec", address: 0, bus: { type: "hda-codec", devices: [
    { type: "hda-audio-group", bus: { type: "hda-audio-group", devices: [
        { type: "hda-output" },
    ] } },
] } }
```

#### `hda-audio-group` — HDA audio function group

Placement: `hda-codec` bus. No properties. Provides an `hda-audio-group` bus
for its outputs.

```js
/* hda-codec bus */
{ type: "hda-audio-group", bus: { type: "hda-audio-group", devices: [
    { type: "hda-output" },
] } }
```

#### `hda-output` — HDA output converter and pin

Placement: `hda-audio-group` bus. Each output plays to its own host back
end.

| Property | Default | Meaning |
| --- | --- | --- |
| `kind` | `"line-out"` | `"line-out"`, `"speaker"` or `"headphone"`. |
| `location` | follows `kind` | `"rear"`, `"front"`, `"left"`, `"right"`, `"top"`, `"bottom"` or `"internal"` (a fixed pin; anything else is a jack). |
| `association` | `1` | 1-15. Pins in one association form one device. |
| `sequence` | `0` | 0-15. Order of the pin within its association. |
| `channels` | `2` | 1-16. |
| `plugged` | `1` | Whether something is plugged into the jack. |
| `rates` | the host rate | Sample rates offered, e.g. `[44100, 48000]`. |
| `host` | host audio | See [audio back end](#audio-back-end-host). |

```js
/* hda-audio-group bus */
{
    type: "hda-output",
    kind: "speaker",
    location: "internal",
    host: { driver: "host", latency: 40 },
}
```

#### `ich-ac97` — Intel ICH AC'97 controller

Placement: PCI, on a bus with I/O port space. No properties. Provides an
`ac97` link.

```js
/* pci bus */
{
    type: "ich-ac97",
    bus: { type: "ac97", devices: [ { type: "ac97-codec" } ] },
}
```

#### `ac97-codec` — AC'97 codec

Placement: `ac97` link (a primary and one secondary codec). Supports variable
rates from 8000 to 48000 Hz.

| Property | Default | Meaning |
| --- | --- | --- |
| `address` | first free | `0` (primary) or `1` (secondary). |
| `vendor_id` | `0x83847600` | SigmaTel STAC9700, which most drivers know. |
| `host` | host audio | Where playback goes. |
| `input` | none | Where recording comes from: an audio back-end object, as for `host`. Without it, the guest records nothing. |

```js
/* ac97 bus */
{
    type: "ac97-codec",
    host: { driver: "host" },
    input: { driver: "wav", file: "mic.wav", loop: 1 },
}
```

#### `usb-audio` — USB Audio Class 1.0 device

Placement: `usb` bus. A full-speed device that provides a `usb-audio` bus
with up to four outputs, and needs at least one.

| Property | Default | Meaning |
| --- | --- | --- |
| `port` | first free | The parent's port to occupy. |

```js
/* usb bus */
{
    type: "usb-audio",
    bus: {
        type: "usb-audio",
        devices: [
            { type: "usb-audio-output" },
            {
                type: "usb-audio-output",
                kind: "headphone",
                host: { driver: "wav", file: "usb-phones.wav" },
            },
        ],
    },
}
```

#### `usb-audio-output` — USB audio playback interface

Placement: `usb-audio` bus. Each output is a separate playback device in the
guest, with a master mute and volume control.

| Property | Default | Meaning |
| --- | --- | --- |
| `kind` | `"speaker"` | `"speaker"`, `"headphone"` or `"line-out"`. |
| `channels` | `2` | 1-8, 16-bit samples. |
| `rates` | the host rate | Sample rates offered, 8000-96000. |
| `host` | host audio | See [audio back end](#audio-back-end-host). |

```js
/* usb-audio bus */
{ type: "usb-audio-output", channels: 2, rates: [44100, 48000] }
```

### Filesystem

#### `virtio-9p` — virtio 9P filesystem

Placement: FDT or PCI. Shares a host directory, or a remote filesystem over
HTTP when built with `-Dfs_net=enabled`. There is no host directory back end
on Windows yet.

| Property | Default | Meaning |
| --- | --- | --- |
| `file` | required | Host directory or URL. |
| `tag` | required | Mount tag the guest uses. |

```js
{ type: "virtio-9p", file: "/tmp", tag: "/dev/root" }
```

In a Linux guest: `mount -t 9p /dev/root /mnt`.

## 6. Retired device types

These names are still recognised, but only to report what replaces them:

| Old type | Replacement |
| --- | --- |
| `ps2` | An `i8042` carrying a `ps2-keyboard` and a `ps2-mouse` on its `ps2` bus. |
| `ide` | A `pci-ide` inside a PCI host bridge, carrying an `ata-disk` on its `ata` bus. |
