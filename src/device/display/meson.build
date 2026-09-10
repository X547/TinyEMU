display_sources = files('simplefb.cpp')
if x86emu
  display_sources += files('vga.cpp')
endif

emu_libs += static_library('device_display', display_sources, kwargs: lib_kwargs)
