# Device test suite

Characterization tests for temu's emulated devices: one bare metal image per
target, which finds out what machine it is on from the device tree or the
ACPI tables and runs the tests the command line selects. `run.py` builds the
images, runs them on temu for each target and machine, starts the machine
again after a test that killed it, and compares the values tests record with
their golden copies.

Targets:

| Target | Image | Boots as |
| --- | --- | --- |
| `riscv64-m`, `riscv32-m` | `build-riscv*/devtests.bin` | the machine's `bios`, in M-mode |
| `riscv64-s`, `riscv32-s` | the same image | the `kernel` with `sbi: "native"`, in S-mode |
| `i686` | `build-i686/devtests.elf` | a PVH `kernel`, 32 bit protected mode, no paging |
| `x86_64` | `build-x86_64/devtests.elf` | a PVH `kernel`, entering long mode itself |

No firmware is involved, and nothing about the machine is built in: devices,
RAM, the timebase and the way to power off all come from the tables. The
RISC-V image relocates itself to wherever it was loaded.

## Running

```sh
python3 run.py                    # build the images, run every target
python3 run.py riscv64-s i686     # some targets
python3 run.py -t 'uart.*'        # some tests
python3 run.py --selftest         # check the framework and the runner
```

It needs temu built with the test fixtures, which a regular build leaves
out:

```sh
meson setup build-fixtures -Dtest_fixtures=true    # in the TinyEMU root
ninja -C build-fixtures
```

`run.py` looks for `build-fixtures`, `build-linux-fixtures` and
`build-windows-fixtures` in the TinyEMU root, or takes `--temu` or `$TEMU`.
From WSL it can run a Windows `temu.exe`. Building the images needs clang,
lld, llvm-objcopy, meson, ninja and Python 3; WSL and Haiku have been tried.

Options worth knowing:

* `--accel` lets temu run x86 on a hypervisor. By default it runs the
  interpreter, since a hypervisor's interrupt controllers stand in for
  temu's own; `--temu-args` passes anything else.
* `--isolate` gives each test a machine of its own.
* `--timeout N` is how long a run may go without a line of output before it
  is killed, 30 seconds by default.
* `--update-golden` records this run's snapshots as the golden copies.
* `--no-fixtures` runs on a temu without the test-control device. The report
  then goes over the UART, and tests that need the fixtures skip.
* `-v` adds the log lines of failing tests and lists tests with no golden
  copy.

Each run's console output is in `work/<target>/<machine>/temu.log`, and
everything is in `work/results.json`.

## Coverage

```sh
python3 run.py --coverage            # run everything, report, compare
python3 run.py --coverage -v         # with the table for every file
```

`--coverage` runs on `build-coverage` in the TinyEMU root, a temu built by
clang with source based coverage and the test fixtures, which it configures
the first time. Each temu process writes its own profile as it runs, so a
run killed after a timeout counts as well. The profiles are merged into
`work/coverage/summary.txt` and an HTML report in `work/coverage/html`, for
the files `coverage-scope.txt` names.

A whole run (all targets, machines and tests) is compared with
`coverage/baseline-<host>.txt`, and a file whose line coverage fell by more
than two points fails the run. `--update-coverage-baseline` records the new
figures once more tests cover more. The baselines are per host because the
compilers count differently and x86 runs on different backends.
`--coverage-keep` adds to the profiles of earlier runs, to sum up runs with
different `--temu-args`.

## Machines

`machines/*.cfg` are temu configurations with `@...@` placeholders that
`run.py` fills in for each target: the boot method, the command line, the
root bus and the test-control device, which `@FIXTURE(irqs: [5, 7])@` gives
properties. `@FIXTURE:pci-test(slot: 3)@` puts in another fixture, here the
PCI test function. The first comment names the targets a machine is for. A
test that needs a device the machine lacks skips.

The fixtures, in temu's `src/device/test`:

* `test-control`: the report stream, the exit status, and interrupt lines
  the guest drives.
* `pci-test`: a PCI function with BARs of every kind (32 and 64 bit
  memory, I/O, an expansion ROM), an INTx pin, MSI and MSI-X it sends on
  request, and DMA to RAM, for testing the buses and host bridges.

## Writing tests

```cpp
#include "test.h"

TEST(uart, scratch_register)
{
    const HwDevice *uart = t_require_device("ns16550a");  /* or skip */
    ...
    EXPECT_EQ(value, 0x55);      /* reported, the test goes on */
    ASSERT_NE(base, 0u);         /* reported, the test ends */
    SNAP("lsr", "%#x", lsr);     /* compared with golden/uart.scratch_register.txt */
}
```

Tests run in name order with interrupts off. Whatever a test allocates from
the heap is released after it, and a test that traps is reported as crashed
while the next one runs on. Each file in `tests/` goes in `meson.build`.

Where the hardware's specification says what a register reads or a device
does, a test asserts that, naming the document. Snapshots are for the rest,
what the specification leaves to the implementation: a test records what
the device does today, `--update-golden` writes it to `golden/<test>.txt`,
and any later difference fails the test until it is looked at. A snapshot
must not depend on timing; a test that measures time brackets each sample
with the clock rather than trusting a fixed window, and skips when the
machine is too slow to tell. Where runs
differ, the copy is looked for in `golden/<machine>/<target>/`, then
`golden/<target>/`, then `golden/<machine>/`, then `golden/`, and
`--update-golden` writes the least particular copies that hold.

A test that records reset values is a `FRESH_TEST`: when other tests ran
before it in the same machine, it answers `RERUN` and `run.py` runs it again
in a machine of its own.

Between tests the runner turns interrupts off, lowers the test-control
lines and resets every interrupt controller that was used, on every
processor that was started. What else a test changes that would reach the
next one it puts back with `t_on_cleanup(fn)`, which runs however the test
ended. `irq_route(device, index, handler, ctx)` delivers a device's
interrupt through whichever controller the tables name, with the trigger
mode they give; `irq_route_isa()` does the same for the PC's fixed ISA
lines. The interrupt controller tests drive their registers themselves,
with the test-control lines as sources. `t_trap(fn)` runs code that is
meant to trap.

PCI is found and set up as firmware would, the first time a test asks for
it: `pci_enumerate()` numbers the buses of every host bridge in the tables
(ECAM, DesignWare, PLDA, or the PC's ports 0xcf8 and 0xcfc), places every
BAR and bridge window in the host's apertures and turns decoding on.
`pci_require(vendor, device)` gives a function or skips; `pci_bar_read()`
reaches its registers through the host's windows, and
`pci_intx_route()` and the host's `msi` sink deliver its interrupts as the
tables say. A test that changes configuration space puts it back with
`pci_restore_all()`. Where a read may find nothing, `pci_space_probe()`
survives the fault an unclaimed address raises on RISC-V.

A virtio device is found by `virtio_require(id, &dev)`, on MMIO from the
tables or on PCI by its IDs and capabilities, and reached the same way on
either transport. `virtio_negotiate()` takes it through feature
negotiation, and `virtq_setup()` gives it a split virtqueue whose
descriptors a test fills in itself.

`smp_run(cpu, fn, ctx)` runs a function on another processor, which is
started the first time: by INIT and STARTUP on x86, by its software
interrupt in M-mode and by the SBI in S-mode. `t_require_other_cpu()` gives
one or skips the test.

The x86 image quiets the PIT, which counts from reset, and sets up the
8259s before the first test, as firmware would; the 8259s' reset state is
kept for `i8259.reset_state`.

A test that stops the machine on purpose calls `t_expect_exit(status)`
first, and passes when temu exits with that status. `t_host_input(buf,
len)` has the runner type the bytes on temu's console, which hands them
to whichever device takes console input.

`known-failures.txt` lists failures that should not fail a run.

## The protocol

The image writes one line per event, `@@T ` first, to the test-control
device, which temu writes to its standard error apart from the consoles
the tests drive, or else the console:

```
HELLO 1 target=riscv64 mode=S out=test-control devices=9 ...
BEGIN uart.scratch_register
FAIL uart.cpp:42 expected value == 0x55: 0x54, 0x55
LOG ...
SNAP lsr 0x60
INPUT 68690a
EXIT 3
CRASH illegal instruction (cause 0x2) at pc ... addr ... error ...
END uart.scratch_register FAIL 1234
DONE pass=10 fail=1 skip=2 crash=0
```

The command line (`/chosen/bootargs`, or the PVH command line) selects:
`tests=<globs>`, `skip=<globs>`, `after=<name>` (the tests after that one,
which is how `run.py` resumes), `list=1`, and `meta=1` for the framework's
own tests. A BEGIN without its END means the test stopped the machine or
hung; after an EXIT line the runner compares temu's exit status with the
one named. INPUT is bytes, in hex, for the runner to write to temu's
standard input.

## Layout

| Directory | |
| --- | --- |
| `arch/` | entry, traps, time and setjmp per architecture |
| `lib/` | the freestanding basics: strings, formatting, the heap |
| `hw/` | the device tree and ACPI readers, the device registry, output, power off, interrupt controllers, PCI |
| `framework/` | registering and running tests |
| `tests/` | the tests |
| `machines/` | machine templates |
| `golden/` | snapshot copies |
