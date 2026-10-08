timer_sources = files('clint.cpp')
# The HPET and the 8254 are PC parts: only the pc machine maps them.
if x86emu
  timer_sources += files('hpet.cpp', 'i8254.cpp')
endif
emu_libs += static_library('device_timer', timer_sources, kwargs: lib_kwargs)
