display_sources = files('banshee.cpp', 'simplefb.cpp', 'virtio_gpu.cpp')
if x86emu
  display_sources += files('vga.cpp')
endif

emu_libs += static_library('device_display', display_sources, kwargs: lib_kwargs)
