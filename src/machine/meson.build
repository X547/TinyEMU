machine_sources = files(
  'devices.cpp',
  'machine.cpp',
  'riscv_machine.cpp',
)
if x86emu
  machine_sources += files('x86_machine.cpp')
endif

emu_libs += static_library('machine', machine_sources, kwargs: lib_kwargs)
