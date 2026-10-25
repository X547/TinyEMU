#
# Device test suite: one bare metal image per target that discovers the
# machine from its FDT or ACPI tables and runs the tests the command line
# selects. See README.md.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#

# C only for the assembler sources, which meson hands to the C compiler.
project('devtests', ['c', 'cpp'],
  version: '0.1',
  meson_version: '>= 1.1.0',
  default_options: [
    'cpp_std=c++20',
    'cpp_eh=none',
    'cpp_rtti=false',
    'optimization=2',
    'debug=true',
    'warning_level=2',
    'b_staticpic=false',
    'b_pie=false',
  ],
)

if not meson.is_cross_build()
  error('configure with one of the cross files in cross/')
endif

cpp = meson.get_compiler('cpp')
if cpp.get_id() != 'clang'
  error('the suite is built with clang')
endif

arch = host_machine.cpu_family()
if arch not in ['riscv32', 'riscv64', 'x86', 'x86_64']
  error('unknown target ' + arch)
endif
is_riscv = arch.startswith('riscv')

add_project_arguments(
  '-ffreestanding',
  '-nostdinc++',
  '-fno-threadsafe-statics',
  '-fno-c++-static-destructors',
  '-fno-stack-protector',
  '-fno-omit-frame-pointer',
  '-fvisibility=hidden',
  language: 'cpp',
)
add_project_arguments('-ffreestanding', language: 'c')

# test_control.h is temu's own definition of the device's registers.
inc = include_directories('.', 'arch', 'lib', 'hw', 'framework',
                          '../../src/device/test')

sources = files(
  'main.cpp',
  'framework/runner.cpp',
  'framework/smp.cpp',
  'hw/acpi.cpp',
  'hw/aplic.cpp',
  'hw/ata.cpp',
  'hw/console.cpp',
  'hw/fdt.cpp',
  'hw/i8259.cpp',
  'hw/imsic.cpp',
  'hw/intc.cpp',
  'hw/ioapic.cpp',
  'hw/lapic.cpp',
  'hw/memory.cpp',
  'hw/pci.cpp',
  'hw/plic.cpp',
  'hw/power.cpp',
  'hw/registry.cpp',
  'hw/scsi.cpp',
  'hw/testctl.cpp',
  'hw/virtio.cpp',
  'lib/cxxrt.cpp',
  'lib/divmod.cpp',
  'lib/format.cpp',
  'lib/heap.cpp',
  'lib/klib.cpp',
  'tests/ata/atapi.cpp',
  'tests/ata/disk.cpp',
  'tests/char/htif.cpp',
  'tests/char/uart.cpp',
  'tests/framework/meta.cpp',
  'tests/intc/aplic.cpp',
  'tests/intc/i8259.cpp',
  'tests/intc/imsic.cpp',
  'tests/intc/ioapic.cpp',
  'tests/intc/lapic.cpp',
  'tests/intc/plic.cpp',
  'tests/pci/designware.cpp',
  'tests/pci/i440fx.cpp',
  'tests/pci/msi.cpp',
  'tests/pci/pci.cpp',
  'tests/pci/plda.cpp',
  'tests/power/syscon.cpp',
  'tests/scsi/buslogic.cpp',
  'tests/scsi/cd.cpp',
  'tests/scsi/disk.cpp',
  'tests/platform/acpi.cpp',
  'tests/platform/platform.cpp',
  'tests/platform/smp.cpp',
  'tests/timer/clint.cpp',
  'tests/timer/hpet.cpp',
  'tests/timer/i8254.cpp',
  'tests/timer/mc146818.cpp',
  'tests/virtio/block.cpp',
  'tests/virtio/console.cpp',
  'tests/virtio/scsi.cpp',
  'tests/virtio/virtio.cpp',
)

# The linker is run directly rather than through the compiler driver, which
# on some hosts hands a bare metal link to the host gcc and its idea of a
# default executable (a PIE, or a shared object on Haiku).
link_args = ['-nostdlib', '-static', '--no-dynamic-linker',
             '--build-id=none', '-z', 'noexecstack']

if is_riscv
  # Linked at 0 as a position independent executable: entry.S applies the
  # R_RISCV_RELATIVE relocations for wherever the image was loaded, so no
  # RAM base is assumed. A firmware (M-mode) and a kernel (S-mode with the
  # native SBI) are the same image; it works out which it is.
  sources += files(
    'arch/riscv/arch.cpp',
    'arch/riscv/entry.S',
    'arch/riscv/setjmp.S',
  )
  add_project_arguments('-fpie', language: ['c', 'cpp'])
  inc = [inc, include_directories('arch/riscv')]
  ld_script = meson.current_source_dir() / 'arch/riscv/link.ld'
  link_args += ['-pie', '-z', 'text']
else
  # Linked where the PVH loader puts it, at the physical address of its
  # PT_LOAD segments.
  sources += files(
    'arch/x86/ap.S',
    'arch/x86/arch.cpp',
    'arch/x86/setjmp.S',
    'arch/x86/traps.S',
  )
  inc = [inc, include_directories('arch/x86')]
  if arch == 'x86'
    sources += files('arch/x86/entry32.S')
    ld_script = meson.current_source_dir() / 'arch/x86/link32.ld'
  else
    sources += files('arch/x86/entry64.S')
    ld_script = meson.current_source_dir() / 'arch/x86/link64.ld'
  endif
  link_args += ['--no-pie']
endif

# Whole, because the tests register themselves from objects nothing else
# refers to.
objects = static_library('devtests',
  sources,
  include_directories: inc,
)

lld = find_program('ld.lld')
elf = custom_target('devtests.elf',
  input: objects,
  output: 'devtests.elf',
  command: [lld] + link_args + ['-T', ld_script, '--whole-archive',
                                '@INPUT@', '-o', '@OUTPUT@'],
  depend_files: files(ld_script),
  build_by_default: true,
)

if is_riscv
  # temu loads a RISC-V firmware or kernel as a flat binary. The check fails
  # the build if anything but a relative relocation slipped in, which the
  # entry code would not apply.
  python = find_program('python3')
  readelf = find_program('llvm-readelf', 'readelf')
  objcopy = find_program('llvm-objcopy', 'objcopy')
  custom_target('devtests.bin',
    input: elf,
    output: 'devtests.bin',
    command: [python, files('tools/flatten.py'), readelf, objcopy,
              '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
  )
endif
