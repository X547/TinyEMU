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

- x86 system emulator based on KVM

- VirtIO console, network, block device, input and 9P filesystem

- Graphical display with SDL

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
  fs_net          network filesystem, needs libcurl and libcrypto
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
-no-accel         disable VM acceleration (KVM, x86 machine only)

Console keys:
Press C-a x to exit the emulator, C-a h to get some help.

3.3 Configuration file
----------------------

The configuration file is JSON with relaxed syntax (unquoted keys, trailing
commas and /* */ comments are accepted). Format version 2 declares devices
inside the bus they attach to; see sample-riscv64.cfg for a commented
example.

The root bus of an FDT machine is declared as:

bus: { type: "fdt", devices: [ ... ] }

Each entry of "devices" is an object with a "type" and whatever properties
that type needs. A device that provides a bus of its own carries a nested
"bus" object, so buses can be nested arbitrarily, for example
FDT bus -> pci-host-ecam-generic -> PCI bus -> virtio device.

Device types:

  ns16550a               serial port; also supplies /chosen/stdout-path
  simplefb               "width", "height"
  pci-host-ecam-generic  ECAM PCIe host bridge; "bus_count" (ECAM window
                         size in MB, default 16), "mmio_size" (aperture size
                         in MB, default 256), "mmio64_size" (size in MB of a
                         second aperture above 4 GB, default 4096; 0 for
                         none), and a nested PCI bus
  pci-host-designware    Synopsys DesignWare PCIe root complex; "mmio_size",
                         "mmio64_size" and "bus_count" as above (bus_count
                         defaults to 16 and bounds only what the device tree
                         advertises), "compatible" (which controller it
                         claims to be, default "sifive,fu740-pcie"), and a
                         nested PCI bus
  pci-bridge             PCI Express switch; the devices nested in it sit
                         behind it rather than on the bus above. Nest one
                         inside another for a deeper hierarchy
  virtio-block           "file"
  virtio-9p              "file", "tag"
  virtio-net             "driver" ("user" or "tap"), "ifname" for tap
  virtio-console         uses the emulator console
  virtio-input           "kind" ("keyboard", "mouse" or "tablet")
  xhci                   USB 3.0 host controller on PCI; "usb2_ports"
                         (default 4) and "usb3_ports" (default 2), and a
                         nested USB bus
  usb-hub                USB 2.0 hub; "ports" (default 4), "port" (which port
                         of the parent to occupy, default the first free one),
                         and a nested USB bus
  usb-storage            USB mass storage, bulk-only transport; "port" as
                         above, and a nested SCSI bus
  usb-hid                USB HID class device, the HID bus over USB; "port" as
                         above, and a nested HID bus
  hid-keyboard           HID keyboard; "index" (which function of the
                         transport to be, default the first free one)
  hid-tablet             HID absolute pointing device; "index" as above
  scsi-disk              SCSI direct access block device; "file", and "lun"
                         (default the first free logical unit)
  nvme                   NVM Express controller on PCI; "quirks" (see below)
                         and a nested NVMe bus
  nvme-ns                NVMe namespace; "file", and "nsid" (default the
                         first free namespace id)
  sdhci                  SD host controller, on PCI or on the FDT bus;
                         "clock" (the base clock in MHz, default 50),
                         "compatible" (which controller the device tree node
                         claims to be, default "arasan,sdhci-8.9a", and
                         unused on PCI), and a nested SD bus
  sd-card                SD memory card; "file", and "read_only"
  mmc-card               eMMC storage device; "file", and "read_only"
  dwmac                  Synopsys DesignWare Ethernet QoS MAC on the FDT bus;
                         "driver" and "ifname" as for virtio-net,
                         "compatible" (which core the node claims to be,
                         default "snps,dwmac-5.10a"; it must name a 4.x or
                         5.x core, because that is the register layout this
                         models), "phy_mode" (default "rgmii-id"), "quirks"
                         (see below), and a nested MDIO bus
  ethernet-phy           generic 10/100/1000 PHY on an MDIO bus; "reg" (its
                         address, default the first free one) and "phy_id"
                         (the identifier the guest reads, default one no
                         vendor owns so that a generic driver binds)

The virtio devices work on either transport: attached to the FDT bus they
appear as virtio-mmio, and attached to a PCI bus they appear as PCI devices.

The USB stack nests the same way everything else does, and the whole path from
the PCI bus down to the image file is spelled out in the file:

    PCI bus -> xhci -> USB bus -> usb-storage -> SCSI bus -> scsi-disk

A hub may be inserted between the controller and a device, which puts that
device one tier further down; the controller reaches it by the route string
the guest programs, exactly as real hardware does. The controller advertises
both a USB 2.0 and a USB 3.0 port set. The device models are high speed, so
they occupy the USB 2.0 ports; the SuperSpeed ports exist because a controller
without them is a shape some guest drivers handle poorly.

A "usb-storage" with no SCSI device below it is an error rather than an empty
drive, and the bus type named in each nested "bus" object is checked against
the bus the device above it actually provides.

Input devices nest the same way:

    PCI bus -> xhci -> USB bus -> usb-hid -> HID bus -> hid-keyboard

A HID function is a report descriptor and the reports that go with it, and
knows nothing about the transport carrying it, so the planned I2C and SPI
transports will take the same "hid-keyboard" and "hid-tablet" nodes. A
"usb-hid" carries up to four of them, each on an interface and an interrupt
endpoint of its own; a guest whose driver binds one function per USB device
rather than per interface needs a "usb-hid" for each instead.

Whichever devices are realized last holding the keyboard and the pointer roles
are the ones the emulator window sends its events to, so a configuration
should declare either "virtio-input" or HID functions, not both.

The NVMe controller nests the same way, down to the image file:

    PCI bus -> nvme -> NVMe bus -> nvme-ns

It behaves as the specification describes unless the configuration asks
otherwise. "quirks" is an array of names, and a name that is not one of these
is reported rather than ignored:

  no-enable-check      serve the admin queue as soon as AQA, ASQ and ACQ have
                       been programmed, instead of waiting for CC.EN
  nsid-zero            take namespace id 0 to mean namespace 1
  loose-queue-create   accept a queue creation that asks for a discontiguous
                       queue or names completion queue 0, and clamp a read
                       that runs off the end of the namespace instead of
                       failing it
  poll-only            never drive the INTx pin, leaving the guest to poll
  haiku                all four, which is what Haiku's RISC-V boot loader and
                       disk driver between them need

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
descriptors, Auto CMD12 and Auto CMD23, and offers MSI-X on a PCI bus that
has a receiver for it. Cards move data one block at a time and may answer
later, so a block back end that does not complete at once -- the HTTP one --
stalls the controller rather than the emulator.

Both card types report 512 byte blocks and do not offer partial ones, so
SET_BLOCKLEN takes 512 and nothing else. Neither claims the erase command
class, so a guest discards nothing rather than being told a discard happened
that did not. A card image larger than two gigabytes becomes a high capacity
card, addressed in blocks; a smaller one is addressed in bytes and holds
what its capacity fields can express, which for an image whose size is not a
round number of allocation units is a little less than the file.

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

The MAC is a bare DesignWare core unless the configuration asks otherwise.
"quirks" is an array of names, and a name that is not one of these is
reported rather than ignored:

  clocks           emit the clocks a StarFive style platform binding names,
                   as one fixed clock standing in for all of them
  link-on-reset    report the link again after a software reset
  haiku            both, which is what Haiku's driver needs together

Both exist for a driver written against a part that wraps this core in a
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
than on the core, so it also wants the name:

    { type: "dwmac", driver: "user",
      compatible: "starfive,jh7110-eqos-5.20", quirks: ["haiku"],
      bus: { type: "mdio", devices: [ { type: "ethernet-phy", reg: 0 } ] } }

The node keeps "snps,dwmac" as its last compatible entry whatever the first
one says, so a generic driver still binds to it.

The two PCI host bridges differ in more than their register layout. The ECAM
one is a bare bus: devices sit on bus 0 and interrupt over INTx. The
DesignWare one models a real root complex, so it has a root port of its own on
bus 0 and devices are enumerated behind it, and it carries a message signalled
interrupt receiver. Devices on a bus that has one advertise MSI-X and use it
in preference to INTx; on a bus without one they do not offer it at all,
because a guest that chose it would have nothing to collect the message.

Which driver binds to the DesignWare bridge is decided by "compatible". The
default names the SiFive FU740, which is what Haiku's DesignWare bus driver
probes for. Linux's driver for that part wants clocks, resets and GPIOs that
this machine does not model, so for Linux ask for "snps,dw-pcie" instead and
its generic DesignWare host driver binds.

Several host bridges may be declared side by side. Each reserves its own
windows, takes its own interrupt lines and is described as a PCI domain of
its own, so a guest names the devices behind them unambiguously.

3.4 PCI topology
----------------

Both host bridges present a PCI Express hierarchy: every function on one
carries a PCI Express capability, and so has the full 4096 byte configuration
space rather than the conventional 256 bytes. A guest reads the extended
capability chain at offset 0x100 like any other; what it finds there is a
device serial number, which every function has one of.

Where a device may sit is decided by the hierarchy, exactly as on hardware.
The ECAM bridge's bus 0 is a root complex bus and holds as many devices as it
has slots. A link, on the other hand, carries one device, so the bus behind
the DesignWare root port and the bus behind any port of a switch each hold
one. A configuration that names several devices in such a place is given the
switch that has to sit between them: an upstream port, the bus inside it, and
a downstream port for each device. That is what "pci-bridge" is, and it is
also what the DesignWare bridge does with the devices declared under it, so
a configuration written against the older bare-bus behaviour keeps working
and simply finds its devices two tiers further down than it used to.

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
"riscv-pk" boot loader to support it. The HTIF console is only used to
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

4.4) x86 emulator

A small x86 emulator is included. It is not really an emulator because
it uses the Linux KVM API to run the x86 code at near native
performance. The x86 emulator uses the same set of VirtIO devices as
the RISCV emulator and is able to run many operating systems.

The x86 emulator accepts a Linux kernel image (bzImage). No BIOS image
is necessary.

The x86 emulator comes from my JS/Linux project (2011) which was one
of the first emulator running Linux fully implemented in
Javascript. It is provided to allow easy access to the x86 images
hosted at https://bellard.org/jslinux .


5) License / Credits
--------------------

TinyEMU is released under the MIT license. If there is no explicit
license in a file, the license from MIT-LICENSE.txt applies.

The SLIRP library has its own license (two clause BSD license).
