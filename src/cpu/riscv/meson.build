# riscv_cpu.cpp is compiled once per supported XLEN.
riscv_cpu_libs = []
xlens = [32, 64]
if max_xlen == 128
  xlens += 128
endif

foreach xlen : xlens
  riscv_cpu_libs += static_library('riscv_cpu@0@'.format(xlen),
    files('riscv_cpu.cpp'),
    cpp_args: common_args + ['-DMAX_XLEN=@0@'.format(xlen)],
    include_directories: inc,
    dependencies: common_deps,
  )
endforeach
