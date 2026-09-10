#!/bin/bash
# Build the riscv-tests ISA suite against TinyEMU's HTIF address and run each
# test, using the emulator's exit status as the verdict: 0 is a pass, anything
# else is the number of the TEST_CASE that failed.
#
# Usage: ./run.sh [suite ...]        default: the suites in $default_suites
#        ./run.sh --clone            fetch riscv-tests next to this script
#
# Environment: RISCV_TESTS, TEMU, CLANG, OBJCOPY, WORK, TIMEOUT, VERBOSE
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

RISCV_TESTS=${RISCV_TESTS:-$here/riscv-tests}
TEMU=${TEMU:-$root/build-linux/temu}
CLANG=${CLANG:-clang}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
WORK=${WORK:-$here/work}
TIMEOUT=${TIMEOUT:-10}
VERBOSE=${VERBOSE:-0}

default_suites="rv64ui rv64um rv64ua rv64uc rv64uf rv64ud rv64mi rv64si"
repo=https://github.com/riscv-software-src/riscv-tests.git

if [ "${1-}" = "--clone" ]; then
    exec git clone --depth 1 --recurse-submodules --shallow-submodules \
        "$repo" "$RISCV_TESTS"
fi

if [ ! -d "$RISCV_TESTS/isa" ]; then
    echo "riscv-tests not found at $RISCV_TESTS" >&2
    echo "run '$0 --clone', or point RISCV_TESTS at an existing checkout" >&2
    exit 1
fi
if [ ! -x "$TEMU" ]; then
    echo "temu not found at $TEMU; set TEMU to the emulator binary" >&2
    exit 1
fi

mkdir -p "$WORK" || exit 1

cat > "$WORK/machine.cfg" <<'EOF'
{
    version: 2,
    machine: "riscv64",
    memory_size: 16,
    bios: "test.bin",
    bus: { type: "fdt", devices: [ {type: "virtio-console"} ] },
}
EOF

# Reasons are only shown for tests that fail, so the whole line is kept.
known_failure() {
    grep -qE "^$1[[:space:]]" "$here/known-failures.txt"
}
known_reason() {
    sed -nE "s/^$1[[:space:]]+//p" "$here/known-failures.txt"
}

cflags="--target=riscv64-unknown-none-elf -march=rv64imafdc -mabi=lp64d
        -mcmodel=medany -mno-relax"

# Preprocess and assemble as two steps rather than letting clang drive both.
#
# riscv_test.h declares {m,s}tvec_handler .weak; a test that supplies a handler
# then declares the same symbol .global. GNU as accepts the binding change,
# clang's integrated assembler rejects it. The redundant .global has to be
# dropped from the *preprocessed* text, not from the test source: the rv64mi
# variants of these tests #include their rv64si counterpart and rename the
# symbol with a #define, so the declaration is not visible in the file named on
# the command line.
build() {
    src=$1
    suite=$2
    $CLANG $cflags -E \
        -I "$RISCV_TESTS/env/p" -I "$RISCV_TESTS/env" \
        -I "$RISCV_TESTS/isa/macros/scalar" -I "$RISCV_TESTS/isa/$suite" \
        "$src" -o "$WORK/test.s" || return 1
    sed -i -E '/^[[:space:]]*\.globa?l[[:space:]]+[ms]tvec_handler[[:space:]]*$/d' \
        "$WORK/test.s" || return 1
    $CLANG $cflags -x assembler -nostdlib -fuse-ld=lld -static \
        -T "$here/link.ld" -o "$WORK/test.elf" "$WORK/test.s" || return 1
    $OBJCOPY -O binary "$WORK/test.elf" "$WORK/test.bin"
}

suites=${*:-$default_suites}
pass=0; fail=0; known=0; unexpected=0; broke=0

for suite in $suites; do
    if [ ! -d "$RISCV_TESTS/isa/$suite" ]; then
        echo "  no such suite: $suite" >&2
        broke=$((broke + 1))
        continue
    fi
    s_pass=0; s_fail=0; s_known=0; s_broke=0
    for src in "$RISCV_TESTS/isa/$suite"/*.S; do
        [ -e "$src" ] || continue
        name="$suite-p-$(basename "$src" .S)"

        if ! build "$src" "$suite" > "$WORK/build.log" 2>&1; then
            echo "  BUILD-FAIL $name"
            [ "$VERBOSE" = 1 ] && sed -n '1,5p' "$WORK/build.log"
            s_broke=$((s_broke + 1))
            continue
        fi

        ( cd "$WORK" && timeout "$TIMEOUT" "$TEMU" machine.cfg ) \
            < /dev/null > "$WORK/run.log" 2>&1
        status=$?

        if [ "$status" = 0 ]; then
            if known_failure "$name"; then
                echo "  UNEXPECTED-PASS $name (remove it from known-failures.txt)"
                unexpected=$((unexpected + 1))
            fi
            s_pass=$((s_pass + 1))
        elif known_failure "$name"; then
            echo "  known fail  $name: $(known_reason "$name")"
            s_known=$((s_known + 1))
        elif [ "$status" = 124 ]; then
            echo "  TIMEOUT     $name (${TIMEOUT}s)"
            s_fail=$((s_fail + 1))
        else
            echo "  FAIL        $name (failing TEST_CASE $status)"
            s_fail=$((s_fail + 1))
        fi
    done
    printf '%-8s pass=%-4d fail=%-3d known-fail=%-3d build-fail=%d\n' \
        "$suite" "$s_pass" "$s_fail" "$s_known" "$s_broke"
    pass=$((pass + s_pass)); fail=$((fail + s_fail))
    known=$((known + s_known)); broke=$((broke + s_broke))
done

echo "===="
printf 'TOTAL    pass=%-4d fail=%-3d known-fail=%-3d build-fail=%d\n' \
    "$pass" "$fail" "$known" "$broke"

[ "$unexpected" != 0 ] && echo "$unexpected test(s) passed unexpectedly"
[ $((fail + broke)) = 0 ] && exit 0
exit 1
