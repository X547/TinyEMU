TinyEMU System Emulator by Fabrice Bellard
==========================================

1) Features
-----------

- RISC-V system emulator supporting the RV128IMAFDQC base ISA (user
  level ISA version 2.2, priviledged architecture version 1.10)
  including:

  - 32/64/128 bit integer registers
  - 32/64/128 bit floating point instructions
  - Compressed instructions
  - dynamic XLEN change

- x86 system emulator based on KVM, with an i686 interpreter (x87 FPU,
  MMX, SSE, SSE2) when KVM is not available

- VirtIO console, network, block device, input, 2D GPU and 9P filesystem

- Graphical display with SDL, or a native window on Haiku, Windows and
  Wayland

- JSON configuration file

- Remote HTTP block device and filesystem

- small code, easy to modify, no external dependancies

2) Installation
---------------

- Meson and Ninja are needed to build the project. The libraries libcurl,
  OpenSSL and SDL 1.2 are optional. On a Fedora system you can install
  everything with:

  sudo dnf install meson ninja-build openssl-devel libcurl-devel SDL-devel

- Configure and build:

  meson setup build
  meson compile -C build

- Build options (pass them to 'meson setup' as -Dname=value, or change
  them later with 'meson configure build -Dname=value'):

  sdl             SDL 1.2 graphical display (feature, default auto)
  haiku_gui       native Haiku window instead of SDL, Haiku hosts only
                  (feature, default auto; -Dsdl=enabled keeps SDL)
  win32_gui       native Win32 window instead of SDL, Windows hosts only
                  (feature, default auto; -Dsdl=enabled keeps SDL)
  wayland_gui     Wayland window instead of SDL, needs libwayland-client,
                  wayland-scanner, wayland-protocols, FreeType and
                  fontconfig (feature, default disabled; cannot be combined
                  with -Dsdl=enabled)
  fs_net         network filesystem, needs libcurl and libcrypto
                  (feature, default disabled)
  builtin_crypto  use the bundled AES/SHA256 code instead of libcrypto
                  (boolean, default false)
  x86emu          build the x86 emulator (boolean, default true)
  slirp           build the user space network redirector
                  (boolean, default true)
  int128          build the 128 bit RISCV target; the compiler must
                  support the __int128 C extension, so this does not work
                  on 32 bit hosts (boolean, default false)

  For example, to build with the network filesystem enabled and without
  the x86 emulator:

  meson setup build -Dfs_net=enabled -Dx86emu=false

- On Windows 8 or later, build with MinGW-w64 GCC the same way. There is
  no tap or 9P host directory there yet.

- You can optionally install the programs with:

  meson install -C build

3) Usage
--------

3.1 Quick examples
------------------

Note: the configuration files below are in the version 1 format, which is
no longer supported. Use them as a source of images and write a version 2
configuration for them; see sample-riscv64.cfg.

- Use the VM images available from https://bellard.org/jslinux (no
  need to download them):

  Terminal:

  ./temu https://bellard.org/jslinux/buildroot-riscv64.cfg

  Graphical (with SDL):

  ./temu https://bellard.org/jslinux/buildroot-x86-xwin.cfg

  ./temu https://bellard.org/jslinux/win2k.cfg

- Download the example RISC-V Linux image
  (diskimage-linux-riscv-yyyy-mm-dd.tar.gz) and use it:

  ./temu root-riscv64.cfg

  ./temu rv128test/rv128test.cfg

- Access to your local hard disk (/tmp directory) in the guest:

  ./temu root_9p-riscv64.cfg

then type:
mount -t 9p /dev/root /mnt

in the guest. The content of the host '/tmp' directory is visible in '/mnt'.

3.2 Invocation
--------------

usage: temu [options] config_file
options are:
-m ram_size       set the RAM size in MB
-rw               allow write access to the disk image (default=snapshot)
-ctrlc            the C-c key stops the emulator instead of being sent to the
                  emulated software
-append cmdline   append cmdline to the kernel command line
-no-accel         run the processors in the interpreter, not in a
                  host hypervisor
-client-decorations
                  draw TinyEMU's own window frame even when the
                  compositor offers one (Wayland)

The Wayland window uses the compositor's frame when it offers one, and
otherwise draws its own: the close button on the left of the title tab,
dragging the tab or the frame moves the window, double clicking them
minimizes it, and the bottom right corner resizes it.

Console keys:
Press C-a x to exit the emulator, C-a h to get some help.

3.3 Configuration file
----------------------

The configuration file is JSON with relaxed syntax (unquoted keys, trailing
commas and /* */ comments are accepted). Format version 2 declares devices
inside the bus they attach to; see sample-riscv64.cfg and sample-pc.cfg for
commented examples.

"cpus" sets the number of processors, 1 unless given. A RISC-V machine takes
up to 64; each hart gets its own CLINT timer and software interrupt and a
machine and a supervisor mode PLIC context. The harts take turns on one host
thread, so more of them do not make the guest faster. The PC machine takes
up to 255 with interrupt_controller "apic" and one otherwise; each runs on a
host thread of its own in the hypervisor, and a kernel booted without
firmware finds them all in its MADT.

"interrupt_controller" chooses a RISC-V machine's external interrupt
controller:

  plic         a PLIC (the default)
  aplic        an APLIC delivering directly to the harts, with a machine level
               domain and a supervisor level domain it delegates every source
               to; the harts implement the Smaia and Ssaia CSRs
  aplic-imsic  the same APLIC delivering by MSIs to an IMSIC at each hart,
               which also takes the MSI-X messages of the devices behind a
               pci-host-ecam-generic bridge

Every machine keeps its CLINT for the timers. On the PC machine it chooses
what the processor has besides the two 8259s:

  pic          nothing (the default)
  apic         a local APIC and an IOAPIC at 0xfec00000, which every line
               of the 8259s also reaches; the local APIC takes the MSI-X
               messages of the devices behind the pci-host-i440fx. A
               kernel booted without firmware finds both in its ACPI
               tables

"sbi" chooses who implements the SBI on a RISC-V machine:

  firmware  the "bios", run in M-mode (the default)
  native    the emulator itself: no bios is loaded, and hart 0 starts in
            S-mode with a0 holding its hart id and a1 the device tree, as
            firmware would leave it. It enters the "kernel", loaded at the
            start of RAM, or without one the first cfi-flash, where firmware
            such as EDK2 runs in place. The other harts wait to be started
            through the HSM extension

"accel" chooses what runs the processors, on either machine: "auto" (the
default) a hardware hypervisor when the host has one, and the interpreter
when it has none; "none" always the interpreter, as -no-accel does. A RISC-V
machine runs on Linux KVM on a riscv64 host with the H extension. KVM takes
only riscv64 guests with "sbi" "native", whose SBI the host kernel then
answers, and "plic" or "aplic" (the guest gets no AIA CSRs). Any other
configuration stops with an error rather than falling back, so one that runs
M-mode firmware needs "accel": "none" on such a host. The harts show the
guest the extensions KVM offers, the host's address translation modes and
its timebase frequency, in place of the interpreter's.

"hypervisor_intc" (true unless given) lets a hardware hypervisor provide the
interrupt controllers it can, of whatever kind the machine has. With false
the machine emulates them all itself. On the PC machine that is the local
APICs and the IOAPIC under WHP, and under KVM, whose kernel has them only as
a set, the 8259s and the 8254 too; the guest then sees no KVM paravirtual
interfaces that need the kernel's local APIC (PV EOI, PV IPIs, PV unhalt,
async page faults). An interrupt for a running processor has to stop its
run, so IPIs cost more than with the hypervisor's local APIC. Without a
hypervisor, and with NVMM, which has no interrupt controllers, the machine's
are used whatever the setting.

"hypervisor_cpuid" chooses the processor a hardware hypervisor shows the
PC's guest in CPUID:

  host         the host's (the default)
  interpreter  the interpreter's: a GenuineIntel family 6 processor with
               MMX, SSE and SSE2 and the highest leaf 1, keeping only the
               features the host has too

The second is for old systems that take a new processor, or one that is not
Intel's, for something it is not: BeOS R5 turns SSE on only on Intel
processors, and dies when it then uses SSE on an AMD host. Under WHP every
#GP then exits to the emulator, which makes RDMSR and WRMSR of an Intel MSR
an AMD host lacks read 0 and do nothing instead of faulting.

"hypervisor_pat" chooses the IA32_PAT a hardware hypervisor's processors
start with:

  default      the hypervisor's, the power-on value (the default)
  write-back   every entry write-back

Hypervisors take the guest's memory types from its page tables and PAT, so
a page the guest maps uncached is accessed uncached in host memory. The
second is for systems that do so for a frame buffer and leave PAT alone:
none of the PC's memory is a real device's, so caching it is always safe.
BeOS R5 maps the 3dfx frame buffer uncached, counting on an MTRR to make it
write-combining, and WHP and KVM ignore guest MTRRs. A guest that programs
PAT itself replaces the value.

The root bus is { type: "fdt", devices: [ ... ] } on a RISC-V machine. On
the PC it is { type: "pc", devices: [ ... ] } under firmware, and
{ type: "acpi", devices: [ ... ] } when the machine loads the kernel itself
and describes the devices in the ACPI tables it builds. docs/devices.md
describes how devices and buses nest, every device type and its properties,
and gives an example definition for each; the sections below explain how the
device models behave.

The virtio devices work on either transport: attached to the FDT bus they
appear as virtio-mmio, and attached to a PCI bus they appear as PCI devices.
On the PC that means inside the host bridge, because a PC has no way to reach
a device that is neither on PCI nor at a fixed port:

    pc bus -> pci-host-i440fx -> PCI bus -> virtio-block

The disk of a PC nests one tier further, because the controller and the drive
are separate parts:

    pc bus -> pci-host-i440fx -> PCI bus -> pci-ide -> ATA bus -> ata-disk

Only "i8042", "ns16550a" and "hpet" go straight on the root bus; everything
else a PC carries is a PCI device and belongs inside the bridge. sample-pc.cfg
is a commented example of a whole PC, as sample-riscv64.cfg is of an FDT
machine.

The USB stack nests the same way everything else does, and the whole path from
the PCI bus down to the image file is spelled out in the file:

    PCI bus -> xhci -> USB bus -> usb-storage -> SCSI bus -> scsi-disk

A hub may be inserted between the controller and a device, which puts that
device one tier further down; the controller reaches it by the route string
the guest programs, exactly as real hardware does. The controller advertises
both a USB 2.0 and a USB 3.0 port set. The device models are full or high
speed, so they occupy the USB 2.0 ports; the SuperSpeed ports exist because a
controller without them is a shape some guest drivers handle poorly.

A "usb-storage" with no SCSI device below it is an error rather than an empty
drive, and the bus type named in each nested "bus" object is checked against
the bus the device above it actually provides.

An ATAPI drive is a SCSI device behind an ATA one, so a CD drive on a PC's
IDE controller nests the same way:

    pci-ide -> ATA bus -> atapi -> SCSI bus -> scsi-cd

The same "scsi-cd" works on any SCSI bus, under "virtio-scsi", "buslogic" or
"usb-storage" as well.

Input devices nest the same way:

    PCI bus -> xhci -> USB bus -> usb-hid -> HID bus -> hid-keyboard

A HID function is a report descriptor and the reports that go with it, and
knows nothing about the transport carrying it, so the same "hid-keyboard" and
"hid-tablet" nodes work over USB and over I2C. A "usb-hid" carries up to four
of them, each on an interface and an interrupt endpoint of its own; a guest
whose driver binds one function per USB device rather than per interface
needs a "usb-hid" for each instead.

Over I2C the path runs through the controller and the bus it provides:

    FDT bus -> dw-i2c -> I2C bus -> i2c-hid -> HID bus -> hid-keyboard

An "i2c-hid" carries one function, because the protocol gives a target one
report descriptor; a keyboard and a tablet are two targets. Like an MDIO bus,
an I2C bus is described rather than enumerated: it places the targets that
name no address on the first free one from 0x08, after every address the
configuration names has been taken, and emits them into the device tree. Each
"i2c-hid" also takes an interrupt line of its own from the machine's interrupt
controller, which it holds while the host has something to read, as the
protocol's level triggered line is meant to be. The controller runs commands
as they are queued, so a transfer takes the bus no time; it holds the bus when
its command FIFO runs dry without a STOP, as the drivers expect.

The NVMe controller nests the same way, down to the image file:

    PCI bus -> nvme -> NVMe bus -> nvme-ns

It behaves as the specification describes unless the configuration asks
otherwise with "quirks", which docs/devices.md lists.

A completion queue interrupt is a level: it is asserted while the queue holds
entries the host has not taken, and it clears when the host rings that queue's
head doorbell. A guest whose handler does not drain the queue before returning
will therefore re-enter it forever, which is what "poll-only" exists for.

The SD stack nests the same way, and like the virtio devices the controller
works on either transport:

    PCI bus -> sdhci -> SD bus -> sd-card
    FDT bus -> sdhci -> SD bus -> mmc-card

Attached to a PCI bus the controller is enumerated, and the guest places its
register window; attached to the FDT bus it takes a register window and an
interrupt line and describes itself in the device tree, together with the
fixed clock the drivers for such a part expect to find. Which driver binds to
that node is decided by "compatible", exactly as it is for the DesignWare
host bridge; the default names an Arasan controller, which is a plain SDHCI
part with no platform glue beyond that clock.

A controller has one slot and so carries one card. Which of the three
protocols the bus carries is the card's business rather than the
controller's: "sd-card" speaks SD and "mmc-card" speaks MMC, both over the
same bus and the same host controller, and an SDIO card would be a third
device on that bus -- the command set, the responses and the card interrupt
line it needs are modelled, but no SDIO peripheral is.

The controller implements programmed I/O, SDMA and ADMA2 with 32 or 64 bit
descriptors, Auto CMD12 and Auto CMD23, and offers MSI-X and MSI on a PCI
bus that has a receiver for them. Cards move data one block at a time and may
answer later, so a block back end that does not complete at once -- the HTTP
one -- stalls the controller rather than the emulator.

The DesignWare MMC host carries the same cards, on the FDT bus only:

    FDT bus -> dw-mmc -> SD bus -> mmc-card

It moves data through a 32 word FIFO, by programmed I/O or with its internal
DMA controller, which walks chained or ring descriptors with 32 or 64 bit
addresses. Which of the two it has is fixed when the part is built, and
changes the descriptor format and the DMA register layout, so a guest whose
driver knows only one needs a controller configured with that one. Without
the DMA controller the node says "fifo-mode" for drivers that do not check.
It can send the stop command itself at the end of a transfer. The
node carries a fixed clock for both of its clock inputs; "starfive,jh7110-mmc"
as "compatible" binds the JH7110 driver, which needs nothing more.

Both card types report 512 byte blocks and do not offer partial ones, so
SET_BLOCKLEN takes 512 and nothing else. Neither claims the erase command
class, so a guest discards nothing rather than being told a discard happened
that did not. A card image larger than two gigabytes becomes a high capacity
card, addressed in blocks; a smaller one is addressed in bytes and holds
what its capacity fields can express, which for an image whose size is not a
round number of allocation units is a little less than the file.

HD Audio nests the way the hardware does: a controller, the link it drives,
the codecs on the link, their function groups, and the ports of a group.

    PCI bus -> intel-hda -> HDA link -> hda-codec -> hda-audio-group
            -> hda-output

Each "hda-output" is a converter and the pin it drives, and plays to a host
back end of its own, so one controller may carry, say, a line out to the
speakers and a headphone jack recorded to a file. A driver that follows the
jacks silences the speakers while headphones are plugged in.

A stream moves at the pace of the host end's own clock rather than the
emulator's: the host device asks for frames and the controller fetches them
from the guest's buffers as it does, so the two never drift apart and nothing
is resampled.

USB audio nests like the other USB devices, with its outputs on a bus of its
own:

    PCI bus -> xhci -> USB bus -> usb-audio -> USB audio bus
            -> usb-audio-output

A "usb-audio" is a full speed USB Audio Class 1.0 device carrying up to four
outputs, each a streaming interface the guest sees as a playback device of
its own.

The output has a master mute and volume, and works as real asynchronous
hardware does. The controller delivers the guest's packets in the USB frames
they are scheduled for, the host end's clock drains them from a FIFO, and a
feedback endpoint reports how many frames per USB frame that clock takes, so
the guest sends exactly what the host plays and nothing is resampled.

The Banshee is the display to reach for when a guest is old enough to have a
driver for it and new enough to want its drawing accelerated. Where the
standard VGA is a frame buffer and nothing else, this carries the 2D engine
that goes with one: screen to screen blits, rectangle fills, host to screen
blits and lines, each combining a source, the destination and an 8x8 pattern
through a ternary raster operation, with two clip rectangles and colorkey
ranges that select between four of those operations per pixel. It also has
the 64x64 two bit hardware cursor, which is what keeps a pointer off the
drawing path altogether.

Stretched blits and polygon fills are not drawn, and neither are line styles,
so a stippled line comes out solid. There is no 3D engine and no command
FIFO: a command runs when its register is written or when the launch area is,
and has finished by the time the write returns, so the engine reports itself
idle and its FIFO empty at all times.

The card has no VGA core, which is the one thing a real Banshee has that this
does not. Nothing is shown until a driver turns the video processor on, so a
machine with one of these and nothing else is blank through its firmware and
its boot loader, and the picture appears when the guest's driver sets a mode.
A machine that needs a picture before then wants a "vga" instead.

The NE2000 is the adapter to reach for when a guest is old enough that its
driver list ends well before virtio. It goes either inside a PCI host bridge,
where it is the RTL8029AS every NE2000 driver knows, or straight on the pc
bus, where it is the ISA card jumpered to the ports and the line "reg" and
"irq" name; a machine that addresses its devices neither way has nowhere to
put one, and says so. Both are the same DP8390 behind the same 32 KB of
buffer memory, and nothing about it is programmed from the machine, so a card
moved off 0x300 or off line 9 has to be told to the guest's driver as well.

The card has no bus master: everything passes through its one data port,
which the guest points at the buffer with a start address and a byte count.
Transmitting hands the buffer straight to the back end, and a frame arriving
from the back end is placed straight into the receive ring. The back end is
asked before it delivers one, so the ring never overflows and the overflow
recovery a driver carries is never reached. The error counters stay at zero,
the multicast hash filter works, and neither the loopback modes nor the byte
order bit is modelled.

The Ethernet MAC nests the same way, and the PHY it talks to is a device of
its own on the MDIO bus the MAC provides:

    FDT bus -> dwmac -> MDIO bus -> ethernet-phy

Unlike a PCI bus, an MDIO bus is enumerated by the machine rather than by the
guest, so it both allocates the addresses and describes its children in the
device tree. A PHY that names no "reg" is placed like any other resource, and
one that does cannot collide with one that does not. A "dwmac" with no PHY
below it is an error rather than a MAC with a dead link.

The MAC takes the same "user" and "tap" back ends virtio-net does, and only
one network device may be declared in a machine: the emulator polls a single
back end from its main loop, so a second would never receive anything.

Descriptors are moved synchronously. Writing a transmit tail pointer drains
the ring there and then, and a frame arriving from the back end is placed
straight into the current receive descriptor; a frame that does not fit one
descriptor is dropped rather than split, because neither reference driver
reassembles one. Checksum offload, segmentation, timestamping, the hash
filter and the statistics counters are all absent, and the feature registers
say so, so a driver never asks for them.

The MAC is a bare DesignWare core unless the configuration asks otherwise
with "quirks" ("clocks" and "link-on-reset"; see docs/devices.md). Both exist for a driver written against a part that wraps this core in a
platform binding rather than for the core itself. Such a driver refuses to
probe when a clock that binding names is missing, and there is no clock
controller in this machine to take one from. And where Linux learns the link
state by polling the PHY over MDIO, a driver may instead learn it only from
the announcement the MAC makes when its in band status changes -- which here
happens before the guest has run, and is then thrown away by the software
reset the driver performs before it starts listening. On real hardware the
two do not collide, because negotiation finishes a second or so after the
driver has started.

Haiku's driver wants both, and its device manager binds on the part rather
than on the core, so it also wants the name; docs/devices.md has the
definition. The node keeps "snps,dwmac" as its last compatible entry whatever the first
one says, so a generic driver still binds to it.

The PCI host bridges differ in more than their register layout. The ECAM one
is a bare bus: devices sit on bus 0 and interrupt over INTx. The DesignWare
and PLDA ones model real root complexes, so each has a root port of its own on
bus 0 and devices are enumerated behind it, and each carries a message
signalled interrupt receiver. Devices on a bus that has one advertise MSI-X and plain
MSI, and a guest uses one of them in preference to INTx; on a bus without one
they offer neither, because a guest that chose either would have nothing to
collect the message. Which of the two a guest takes is its own choice, and it
takes MSI-X wherever it is offered. A virtio device offers MSI-X alone: its
per queue vector registers are part of that capability, and a driver without
it uses the ISR register and the INTx line instead.

Which driver binds to the DesignWare bridge is decided by "compatible". The
default names the SiFive FU740, which is what Haiku's DesignWare bus driver
probes for. Linux's driver for that part wants clocks, resets and GPIOs that
this machine does not model, so for Linux ask for "snps,dw-pcie" instead and
its generic DesignWare host driver binds.

The PLDA bridge reaches configuration space through a plain ECAM window, and
collects INTx and message signalled interrupts in its own status registers,
leaving on one line. Its device tree numbers the four INTx inputs from 0, by
status bit. The JH7110's own device tree numbers them from 1, which the driver
it shares with the Microchip part does not translate, so INTA would be lost;
the numbering here is the one the driver expects.

Several host bridges may be declared side by side. Each reserves its own
windows, takes its own interrupt lines and is described as a PCI domain of
its own, so a guest names the devices behind them unambiguously.

3.4 PCI topology
----------------

The ECAM, DesignWare and PLDA host bridges present a PCI Express hierarchy:
every function on one carries a PCI Express capability, and so has the full
4096 byte configuration space rather than the conventional 256 bytes. A guest
reads the extended capability chain at offset 0x100 like any other; what it
finds there is a device serial number, which every function has one of.

Where a device may sit is decided by the hierarchy, exactly as on hardware.
The ECAM bridge's bus 0 is a root complex bus and holds as many devices as it
has slots. A link, on the other hand, carries one device, so the bus behind
the DesignWare or PLDA root port and the bus behind any port of a switch each
hold one. A configuration that names several devices in such a place is refused,
and asked for the switch that has to sit between them. That switch is
"pci-bridge": an upstream port, the bus inside it, and a downstream port for
each device nested in it. It is written out rather than inserted silently,
because it is part of the machine the guest will enumerate.

A bridge forwards what falls in one of its windows and nothing else, so a
base address register behind one is reachable only while the memory or I/O
window of every bridge above it covers the place the guest put it. Moving a
window remaps what is behind it. A guest that programmed the windows and the
registers to agree, which is what firmware and every kernel do, sees no
difference; one that did not finds the register silent rather than answering
from an address its own bridges do not carry.

INTx is swizzled at every tier the way a guest expects: a device's pin is
swizzled by its slot at each bridge it passes, and the host bridge's device
tree "interrupt-map" describes only the last step, so the table and the
emulation are derived from the same routing function.

Base address registers may be 64 bits wide, and the NVMe and xHCI controllers
declare theirs that way because their specifications do. Such a register is a
pair, sized and programmed as one, and takes the slot after it. A host bridge
therefore advertises an aperture above 4 GB as well as the 32 bit one, and
firmware that keeps a free list per aperture kind has somewhere to place one.
"mmio64_size" sizes it, or 0 leaves the bridge with only the 32 bit aperture.

A host bridge also advertises an I/O aperture, so that a device whose base
address register asks for port space has somewhere to be placed. A RISC-V
processor has no port instructions, so the ports are reached the way this
machine reaches everything else: the bridge maps a memory window over its
aperture and says so in the device tree, as an "I/O" range whose parent
address is that window and whose child address is the first port. The port
space is one space for the whole machine, allocated like the MMIO space, so
the second host bridge in a machine gets a slice of its own rather than
colliding with the first. "io_size" sizes the aperture in KB, must be a power
of two of at least 4 (the granularity a PCI to PCI bridge forwards I/O in),
and 0 leaves the bridge with no I/O aperture and its port space out of reach.

MMIO addresses and interrupt lines are never written in the configuration
file. They are allocated when the machine is built, checked against each
other and against the architectural ranges, and then described to the guest
in the device tree from the values that were actually assigned. A device
that does not fit, or a PCI aperture that would overlap something else, is
reported instead of silently shadowing another mapping. Apertures above 4 GB
come from a window of their own, but are recorded in the same map, so several
host bridges may each have one and an overlap is still reported.

3.5 Network usage
-----------------

The easiest way is to use the "user" mode network driver. No specific
configuration is necessary.

TinyEMU also supports a "tap" network driver to redirect the network
traffic from a VirtIO network adapter.

You can look at the netinit.sh script to create the tap network
interface and to redirect the virtual traffic to Internet thru a
NAT. The exact configuration may depend on the Linux distribution and
local firewall configuration.

The VM configuration file must include, among the devices of a bus:

{ type: "virtio-net", driver: "tap", ifname: "tap0" }

and configure the network in the guest system with:

ifconfig eth0 192.168.3.2
route add -net 0.0.0.0 gw 192.168.3.1 eth0

The "user" driver translates addresses, so the guest reaches the network but
nothing on the network has an address for the guest. A service the guest
offers is published on a host port instead, with a "forward" entry beside the
"driver" that asks for it:

{ type: "ne2000", driver: "user",
  forward: [ { host_port: 2323, guest_port: 23 } ] }

A connection to port 2323 of the host then arrives at port 23 of the guest.
The port is accepted on 127.0.0.1 unless "host_addr" says otherwise: a forward
is a hole through to the guest, so opening it on "0.0.0.0" -- which offers the
guest to the whole network -- is something the configuration has to ask for.
docs/devices.md lists the other properties of an entry.

Only the "user" driver takes forwards. A "tap" guest is on the host's own
network with an address of its own, and is reached at it.

FTP needs more than its control port forwarded, because the data connection
is made separately and to an address the two ends tell each other over the
control one. Forwarding port 21 gets the session started, and the address
translation rewrites what passes: an FTP client using active mode (the PORT
command, which is what the ftp program shipped with Windows does by default)
works, and one using passive mode works if it ignores the address in the
server's reply and reuses the one it is already connected to, which clients
written for NAT do. Forwarding port 20 as well achieves nothing: in active
mode the guest makes that connection outward rather than accepting one.

3.6 Network filesystem
----------------------

TinyEMU supports the VirtIO 9P filesystem to access local or remote
filesystems. For remote filesystems, it does HTTP requests to download
the files. The protocol is compatible with the vfsync utility. In the
"mount" command, "/dev/rootN" must be used as device name where N is
the index of the filesystem. When N=0 it is omitted.

The build_filelist tool builds the file list from a root directory. A
simple web server is enough to serve the files.

The '.preload' file gives a list of files to preload when opening a
given file.

3.7 Network block device
------------------------

TinyEMU supports an HTTP block device. The disk image is split into
small files. Use the 'splitimg' utility to generate images. The URL of
the JSON blk.txt file must be provided as disk image filename.

4) Technical notes
------------------

4.1) 128 bit support

The RISC-V specification does not define all the instruction encodings
for the 128 bit integer and floating point operations. The missing
ones were interpolated from the 32 and 64 ones.

Unfortunately there is no RISC-V 128 bit toolchain nor OS now
(volunteers for the Linux port ?), so rv128test.bin may be the first
128 bit code for RISC-V !

4.2) Floating point emulation

The floating point emulation is bit exact and supports all the
specified instructions for 32, 64 and 128 bit floating point
numbers. It uses the new SoftFP library.

4.3) HTIF console

The standard HTIF console uses registers at variable addresses which
are deduced by loading specific ELF symbols. TinyEMU does not rely on
an ELF loader, so it is much simpler to use registers at fixed
addresses (0x40008000). A small modification was made in the
"riscv-pk" boot loader to support it. The device tree describes them
too, in the "reg" of its "ucb,htif0" node, which is where OpenSBI looks
for them. The HTIF console is only used to
display boot messages and to power off the virtual system. The OS
should use the VirtIO console.

Power off follows the spike/riscv-tests convention: the guest writes
(code << 1) | 1 to "tohost" with both the device and command fields
zero, and "code" becomes the exit status of the emulator process. A
plain 1 is therefore a successful power off, as before. Codes that do
not fit in the 8 bits a process exit status carries are reported as
255, so that a failing guest is never mistaken for a passing one. This
lets a guest act as an automated test: it reports pass or fail through
the exit status, with no need to grep the console log.

The "syscon-poweroff" device does the same from a single 32 bit register,
with the values of the SiFive test device: 0x5555 powers off with exit
status 0, and (code << 16) | 0x3333 powers off with "code", or 1 when
"code" is 0. Firmware that finds no HTIF registers, such as a generic
OpenSBI, powers off through this device instead.

4.4) x86 emulator

A small x86 emulator is included. When the Linux KVM API is available it
runs the x86 code at near native performance. Otherwise, or with
-no-accel, an interpreter takes its place: a 32 bit i686 with an x87 FPU,
MMX, SSE and SSE2, which is slower but needs nothing from the host. The x86
emulator uses the same set of VirtIO devices as the RISCV emulator and is
able to run many operating systems.

The x86 emulator accepts a Linux kernel image (bzImage), or an ELF kernel
with a PVH entry point (a Linux vmlinux built with CONFIG_PVH), which also
gets the "initrd" and the memory map. No BIOS image is necessary. A 64 bit
kernel needs a hypervisor. Either kind of kernel finds ACPI tables at
0xe0000 describing the PCI host bridge and its interrupt routing, the i8042,
the HPET and a PM timer, and can power the machine off through ACPI.

RAM up to 3 GB starts at address 0; the rest is placed from 4 GB, which
needs a hypervisor, since the interpreter has no PAE. Between the two are the
host bridge's aperture, a window for devices that want host address space,
the IOAPIC, the local APIC and the BIOS. With a hypervisor the processors
report a physical address width of at most 40 bits, and the host bridge's
second aperture runs from the end of RAM up to that width.

The PC is built from the configuration's device tree, as every machine is.
What it has before any device is declared is the part a PC cannot be without:
RAM, the two interrupt controllers, the timer and the clock. Everything else
-- the host bridge, the display, the keyboard controller, the disks -- is
declared, and where a device may sit is decided by the hierarchy: a PCI device
only works inside a "pci-host-i440fx" or "pci-host-cloudhv", and only
"i8042", "ns16550a" and "hpet" go straight on the root bus.

The x86 emulator comes from my JS/Linux project (2011) which was one
of the first emulator running Linux fully implemented in
Javascript. It is provided to allow easy access to the x86 images
hosted at https://bellard.org/jslinux .


5) License / Credits
--------------------

TinyEMU is released under the MIT license. If there is no explicit
license in a file, the license from MIT-LICENSE.txt applies.

The SLIRP library has its own license (two clause BSD license).
