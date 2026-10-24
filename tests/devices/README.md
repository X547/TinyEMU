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

* `--temu-args=-no-accel` runs x86 on the interpreter rather than a
  hypervisor.
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
root bus and the test-control device. The first comment names the targets a
machine is for. A test that needs a device the machine lacks skips.

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

Snapshots are what makes these characterization tests: a test records what
the device does today, `--update-golden` writes it to `golden/<test>.txt`,
and any later difference fails the test until it is looked at. Where runs
differ, the copy is looked for in `golden/<machine>/<target>/`, then
`golden/<target>/`, then `golden/<machine>/`, then `golden/`, and
`--update-golden` writes the least particular copies that hold.

A test that records reset values is a `FRESH_TEST`: when other tests ran
before it in the same machine, it answers `RERUN` and `run.py` runs it again
in a machine of its own.

Between tests the runner turns interrupts off, lowers the test-control
lines and resets every interrupt controller that was used.
`irq_route(device, index, handler, ctx)` delivers a device's interrupt
through whichever controller the tables name; the interrupt controller tests
drive their registers themselves, with the test-control lines as sources.

`known-failures.txt` lists failures that should not fail a run.

## The protocol

The image writes one line per event, `@@T ` first, to the test-control
device, or else the console:

```
HELLO 1 target=riscv64 mode=S out=test-control devices=9 ...
BEGIN uart.scratch_register
FAIL uart.cpp:42 expected value == 0x55: 0x54, 0x55
LOG ...
SNAP lsr 0x60
CRASH illegal instruction (cause 0x2) at pc ... addr ... error ...
END uart.scratch_register FAIL 1234
DONE pass=10 fail=1 skip=2 crash=0
```

The command line (`/chosen/bootargs`, or the PVH command line) selects:
`tests=<globs>`, `skip=<globs>`, `after=<name>` (the tests after that one,
which is how `run.py` resumes), `list=1`, and `meta=1` for the framework's
own tests. A BEGIN without its END means the test stopped the machine or
hung.

## Layout

| Directory | |
| --- | --- |
| `arch/` | entry, traps, time and setjmp per architecture |
| `lib/` | the freestanding basics: strings, formatting, the heap |
| `hw/` | the device tree and ACPI readers, the device registry, output, power off |
| `framework/` | registering and running tests |
| `tests/` | the tests |
| `machines/` | machine templates |
| `golden/` | snapshot copies |
