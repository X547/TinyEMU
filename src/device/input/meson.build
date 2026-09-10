input_sources = files(
  'hid_keyboard.cpp',
  'hid_tablet.cpp',
  'usb_hid.cpp',
  'virtio_input.cpp',
)
if x86emu
  input_sources += files(
    'pckbd.cpp',
    'ps2.cpp',
    'vmmouse.cpp',
  )
endif

emu_libs += static_library('device_input', input_sources, kwargs: lib_kwargs)
