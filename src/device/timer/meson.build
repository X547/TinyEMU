# The HPET is a PC part: only the pc machine maps one.
if x86emu
  emu_libs += static_library('device_timer',
    files('hpet.cpp'),
    kwargs: lib_kwargs,
  )
endif
