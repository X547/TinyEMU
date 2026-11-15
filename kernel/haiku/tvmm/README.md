# tvmm

TinyEMU's hypervisor for Haiku: a kernel driver that publishes
`/dev/misc/tvmm`, which TinyEMU built with the `tvmm` option runs its
processors on. It is built on Haiku itself, against the private kernel
headers of the running system, and checks nothing else about the hrev it is
loaded on.

## Building and installing

```
meson setup build --prefix ~/config/non-packaged
ninja -C build
ninja -C build install
```

The install puts the driver among the add-ons of the prefix, as
`add-ons/kernel/drivers/bin/tvmm`, with the link devfs publishes it by,
`add-ons/kernel/drivers/dev/misc/tvmm`. The prefix is one of the directories
Haiku loads drivers from: `~/config/non-packaged` for the user's own,
`/boot/system/non-packaged`, meson's default on Haiku, for the system's, or
`~/config` and `/boot/system` themselves, which hold what packages install and
are read only outside of a package. `tvmm_smoke` and `tvmm_stress`, the
tests, are built but not installed; each runs on the installed driver.

## Replacing an installed driver

**Nothing may use the device while a new driver installs, nor for a few
seconds afterwards.** Haiku's devfs can panic the system otherwise.

- When the binary of a driver that is loaded changes, devfs reloads it later,
  from a kernel daemon. It does not count an open of a device that was opened
  and closed before, so that reload does not wait for a client that opened
  the device in the meantime: the client's next call can land in the
  unloaded driver.
- The install makes the link again each time, which drops the driver and
  adds it back, also later. A descriptor still open across that is freed
  through the new driver while its objects point into the old one.

So, before `ninja install`, end every program that uses the device: TinyEMU
on tvmm, `tvmm_smoke` and `tvmm_stress`. After it, wait until the driver's
kernel image is a new one before starting any of them again; its ID in

```
listimage 1 | grep drivers/dev/misc/tvmm
```

changes when the reload is done, which takes about a second. A driver that
was not loaded when it was installed needs no wait: the next open loads the
new binary.

Run `sync` before loading or testing a new driver. BFS journals only
metadata, so a kernel crash can lose the contents of files written shortly
before.
