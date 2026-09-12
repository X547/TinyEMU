storage_sources = files(
  'ata_disk.cpp',
  'mmc_card.cpp',
  'nvme.cpp',
  'scsi_disk.cpp',
  'sd_card.cpp',
  'usb_storage.cpp',
  'virtio_block.cpp',
)
emu_libs += static_library('device_storage', storage_sources, kwargs: lib_kwargs)
