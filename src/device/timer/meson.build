timer_sources = files('clint.cpp')
# The HPET is a PC part: only the pc machine maps one.
if x86emu
  timer_sources += files('hpet.cpp')
endif
emu_libs += static_library('device_timer', timer_sources, kwargs: lib_kwargs)
