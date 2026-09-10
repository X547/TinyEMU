storage_sources = files(
  'nvme.cpp',
  'scsi_disk.cpp',
  'usb_storage.cpp',
  'virtio_block.cpp',
)
if x86emu
  storage_sources += files('ide.cpp')
endif

emu_libs += static_library('device_storage', storage_sources, kwargs: lib_kwargs)
