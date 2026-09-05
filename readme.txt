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
                         in MB, default 256), and a nested PCI bus
  pci-host-designware    Synopsys DesignWare PCIe root complex; "mmio_size"
                         (aperture size in MB, default 256), "compatible"
                         (which controller it claims to be, default
                         "sifive,fu740-pcie"), and a nested PCI bus
  virtio-block           "file"
  virtio-9p              "file", "tag"
  virtio-net             "driver" ("user" or "tap"), "ifname" for tap
  virtio-console         uses the emulator console
  virtio-input           "kind" ("keyboard", "mouse" or "tablet")

The virtio devices work on either transport: attached to the FDT bus they
appear as virtio-mmio, and attached to a PCI bus they appear as PCI devices.

The two PCI host bridges differ in more than their register layout. The ECAM
one is a bare bus: devices sit on bus 0 and interrupt over INTx. The
DesignWare one models a real root complex, so it has a root port of its own on
bus 0 and devices are enumerated on bus 1 behind it, and it carries a message
signalled interrupt receiver. Devices on a bus that has one advertise MSI-X
and use it in preference to INTx; on a bus without one they do not offer it at
all, because a guest that chose it would have nothing to collect the message.

Which driver binds to the DesignWare bridge is decided by "compatible". The
default names the SiFive FU740, which is what Haiku's DesignWare bus driver
probes for. Linux's driver for that part wants clocks, resets and GPIOs that
this machine does not model, so for Linux ask for "snps,dw-pcie" instead and
its generic DesignWare host driver binds.

MMIO addresses and interrupt lines are never written in the configuration
file. They are allocated when the machine is built, checked against each
other and against the architectural ranges, and then described to the guest
in the device tree from the values that were actually assigned. A device
that does not fit, or a PCI aperture that would overlap something else, is
reported instead of silently shadowing another mapping.

3.4 Network usage
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

3.5 Network filesystem
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

3.6 Network block device
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
