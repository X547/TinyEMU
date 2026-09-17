subdir('host')

emu_libs += static_library('bus_usb',
  usb_host_sources,
  files(
    'usb.cpp',
    'usb_desc.cpp',
    'usb_hub.cpp',
  ),
  kwargs: lib_kwargs,
)
