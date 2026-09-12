input_sources = files(
  'hid_keyboard.cpp',
  'hid_tablet.cpp',
  'ps2_keyboard.cpp',
  'ps2_mouse.cpp',
  'usb_hid.cpp',
  'virtio_input.cpp',
)
if x86emu
  # The VMware pointer protocol is read out of x86 registers, so it only
  # means anything on the PC machine.
  input_sources += files('vmmouse.cpp')
endif

emu_libs += static_library('device_input', input_sources, kwargs: lib_kwargs)
