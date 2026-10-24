input_sources = files(
  'hid_keyboard.cpp',
  'hid_tablet.cpp',
  'i2c_hid.cpp',
  'ps2_keyboard.cpp',
  'ps2_mouse.cpp',
  'usb_hid.cpp',
  'virtio_input.cpp',
)

emu_libs += static_library('device_input', input_sources, kwargs: lib_kwargs)
