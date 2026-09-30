# ICI benchmark, x86_64

Inter-processor interrupt latency on the `pc` machine, as a bare metal
x86_64 ELF booted through PVH (`kernel:`). It runs on every backend that
supports `cpus` > 1 with `interrupt_controller: "apic"`: WHP, KVM, NVMM and
the interpreter's processor threads.

CPU 0 finds the processors in the ACPI MADT, starts them with INIT and
STARTUP, calibrates the TSC against the ACPI PM timer (host time on every
backend) and then times, with xAPIC fixed IPIs:

* **pingpong cpuN**: an IPI to CPU N, whose handler sends one back to CPU 0.
* **broadcast**: an all-but-self IPI, until every handler has counted in a
  shared counter.

Each is run with the other processors spinning (`spin`) and halted (`halt`).
CPU 0 always waits spinning with interrupts on. The handlers are a few
instructions of assembly in `start.S`, so the numbers are the IPI path itself.

## Running

```sh
./run.sh            # 2 and 4 processors
./run.sh 2 8        # the given counts
TEMU=../../../build-windows/temu.exe ./run.sh 4
TEMU_ARGS=-no-accel ./run.sh 4
```

Building needs clang and lld. Without clang, for example in Git Bash on
Windows, `run.sh` uses the `work/ici.elf` a previous build left, so build in
WSL first. `ITERATIONS` (default 10000, at most 100000) is passed on the
kernel command line as `iterations=N`; `TEMU`, `TEMU_ARGS`, `CLANG`, `WORK`
and `TIMEOUT` override the rest.

temu exits 0 after `ici: done` (S5 power off). A processor that does not
start, an IPI not answered within a second, or an exception prints the reason
and triple-faults, which is exit status 1.

## Example

WHP on an AMD host, 4 processors:

```
ici: 4 cpus, TSC 3593.8 MHz, 10000 iterations
round trip, us            min   median     mean      p99      max
pingpong spin cpu1       8.71     8.86     9.57    15.20   140.52
pingpong spin cpu2       8.71     8.88     9.59    18.57   227.09
pingpong spin cpu3       8.75     8.89     9.39    15.74   131.61
broadcast spin           3.96     4.05     4.33     8.93   153.74
pingpong halt cpu1      10.24    16.74    17.59    30.30   102.38
pingpong halt cpu2      10.49    16.73    17.55    31.43   137.26
pingpong halt cpu3       9.74    16.78    18.24    34.10   172.84
broadcast halt           5.06    15.33    15.97    28.04   192.74
ici: done
```

A ping-pong is two IPIs and a broadcast one IPI plus a memory write, which is
why the broadcast is the faster of the two here.

The interpreter (`-no-accel`), same host and processors:

```
ici: 4 cpus, TSC 100.0 MHz, 10000 iterations
round trip, us            min   median     mean      p99      max
pingpong spin cpu1       3.00     3.00     3.35     5.00    24.00
pingpong spin cpu2       3.00     3.00     3.35     7.00    20.00
pingpong spin cpu3       3.00     3.00     3.30     5.00    25.00
broadcast spin           8.00    15.00    15.18    19.00    36.00
pingpong halt cpu1       2.00     7.00     6.90    13.00    30.00
pingpong halt cpu2       2.00     7.00     6.76    15.00    49.00
pingpong halt cpu3       2.00     7.00     7.06    16.00    33.00
broadcast halt           4.00     9.00     9.54    15.00    37.00
ici: done
```

The interpreter's TSC is host microseconds scaled to 100 MHz, so single
samples have a resolution of 1 us; the means are still finer than that.
