# Host bridges, built into the PCI bus library: they are never useful apart
# from it, and keeping the x86 only one next to its siblings puts the
# condition beside the file it guards.
pci_host_sources = files(
  'pci_host_dw.cpp',
  'pci_host_ecam.cpp',
)
if x86emu
  pci_host_sources += files('pci_host_i440fx.cpp')
endif
