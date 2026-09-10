subdir('host')

emu_libs += static_library('bus_pci',
  pci_host_sources,
  files(
    'pci.cpp',
    'pci_bridge.cpp',
  ),
  kwargs: lib_kwargs,
)
