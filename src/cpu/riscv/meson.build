# riscv_cpuNN.cpp each instantiate the interpreter for one maximum XLEN.
riscv_cpu_files = files('riscv_cpu32.cpp', 'riscv_cpu64.cpp')
if max_xlen == 128
  riscv_cpu_files += files('riscv_cpu128.cpp')
endif

# What runs the interpreter as the machine's harts, built once.
emu_libs += static_library('cpu_riscv',
  riscv_cpu_files + files('riscv_interp_backend.cpp', 'riscv_interp_sbi.cpp'),
  kwargs: lib_kwargs)
