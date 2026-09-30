# ICI benchmarks

Inter-processor interrupt latency, one bare metal payload per architecture and
interrupt controller, each with its own `run.sh`:

* [`x86_64`](x86_64/README.md): xAPIC fixed IPIs on the `pc` machine, booted
  through PVH (WHP, KVM, NVMM, the interpreter).
