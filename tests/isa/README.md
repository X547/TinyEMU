# riscv-tests ISA suite

Runs the RISC-V instruction set tests from
[riscv-tests](https://github.com/riscv-software-src/riscv-tests) against temu,
one emulator invocation per test.

The tests report their result through HTIF rather than the console: a pass
writes 1 to `tohost`, a failure writes `(TEST_CASE << 1) | 1`. temu turns that
into its own exit status (readme.txt section 4.3), so a failing test names the
case that failed without anything having to parse the console output.

## Running

```sh
./run.sh --clone     # once, fetches riscv-tests next to this script
./run.sh             # build and run every suite
./run.sh rv64ui rv64um
```

`run.sh` exits 0 when nothing failed except the tests in `known-failures.txt`.

Requires clang, lld and llvm-objcopy — the tests are pure assembly, so no
riscv64 GCC is needed. The default `TEMU` is `../../build-linux/temu`;
`RISCV_TESTS`, `TEMU`, `CLANG`, `OBJCOPY`, `WORK`, `TIMEOUT` and `VERBOSE`
override the defaults.

## Expected output

```
rv64ui   pass=54   fail=0   known-fail=0   build-fail=0
rv64um   pass=13   fail=0   known-fail=0   build-fail=0
rv64ua   pass=19   fail=0   known-fail=0   build-fail=0
rv64uc   pass=1    fail=0   known-fail=0   build-fail=0
rv64uf   pass=11   fail=0   known-fail=0   build-fail=0
rv64ud   pass=11   fail=0   known-fail=1   build-fail=0
rv64mi   pass=14   fail=0   known-fail=3   build-fail=0
rv64si   pass=7    fail=0   known-fail=0   build-fail=0
====
TOTAL    pass=130  fail=0   known-fail=4   build-fail=0
```

## known-failures.txt

Tests listed there are reported but do not fail the run, so the suite can be
used as a regression gate while the underlying gaps are still open. A listed
test that starts passing is reported as an unexpected pass, so the entry can be
removed.

## What the two local files are for

Upstream expects a runner that loads ELF images and finds `tohost` in the
symbol table. temu loads a flat binary and puts HTIF at a fixed address, so:

* **`link.ld`** replaces `env/p/link.ld`, placing the `.tohost` section on the
  HTIF device at `0x40008000` instead of in RAM.
* **`run.sh`** preprocesses and assembles in two steps so it can drop a
  redundant `.global {m,s}tvec_handler` from the preprocessed text.
  `riscv_test.h` declares those symbols `.weak` and a test supplying a handler
  redeclares the same symbol `.global`; GNU as allows the binding change and
  clang's integrated assembler does not. It has to be patched after
  preprocessing because the `rv64mi` variants `#include` their `rv64si`
  counterpart and rename the symbol with a `#define`, so the declaration never
  appears in the file named on the command line.

## Not covered

Only the `p` (bare machine mode) environment. The `v` variants need a
supervisor environment with virtual memory, and the vector, bitmanip and
half-precision suites need `-march` extensions temu does not implement.

[riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test) is a
separate thing: it runs under RISCOF, which diffs signature dumps against a
reference model (Sail or spike), and would need a DUT plugin plus a signature
dump path in temu.
