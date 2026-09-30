#!/bin/bash
# Build the x86_64 ICI benchmark and run it on each number of processors.
# temu's exit status is the verdict: 0 when every IPI was answered.
#
# Usage: ./run.sh [cpus ...]        default: 2 4
# Environment: TEMU, TEMU_ARGS (e.g. -no-accel), CLANG, WORK, ITERATIONS,
#              TIMEOUT
#
# Without clang (e.g. Git Bash on Windows) the last work/ici.elf is used.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)

TEMU=${TEMU:-$root/build-linux/temu}
TEMU_ARGS=${TEMU_ARGS:-}
CLANG=${CLANG:-clang}
WORK=${WORK:-$here/work}
ITERATIONS=${ITERATIONS:-10000}
TIMEOUT=${TIMEOUT:-600}

mkdir -p "$WORK" || exit 1

if command -v "$CLANG" > /dev/null; then
    "$CLANG" --target=x86_64-unknown-none-elf -ffreestanding -fno-pic \
        -fno-stack-protector -mno-red-zone -mgeneral-regs-only -O2 \
        -Wall -Wextra -nostdlib -static -fuse-ld=lld \
        -Wl,-T,"$here/link.ld" -Wl,--no-dynamic-linker \
        -o "$WORK/ici.elf" "$here/start.S" "$here/ici.c" || exit 1
elif [ ! -f "$WORK/ici.elf" ]; then
    echo "no $CLANG to build $WORK/ici.elf with" >&2
    exit 1
else
    echo "no $CLANG; using the existing $WORK/ici.elf"
fi

status=0
for cpus in ${*:-2 4}; do
    cat > "$WORK/ici$cpus.cfg" <<EOF
{
    version: 2,
    machine: "pc",
    memory_size: 64,
    cpus: $cpus,
    interrupt_controller: "apic",
    kernel: "ici.elf",
    cmdline: "iterations=$ITERATIONS",
    bus: { type: "pc", devices: [ { type: "ns16550a" } ] },
}
EOF
    echo "== $cpus cpus"
    timeout "$TIMEOUT" "$TEMU" $TEMU_ARGS "$WORK/ici$cpus.cfg"
    rc=$?
    [ $rc -eq 0 ] || { echo "exit $rc"; status=1; }
done
exit $status
