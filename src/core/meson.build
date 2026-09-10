# Shared with the tools, which cannot name a file outside their own directory.
cutils_src = files('cutils.cpp')

emu_libs += static_library('core',
  cutils_src,
  files(
    'device.cpp',
    'fdt.cpp',
    'iomem.cpp',
    'json.cpp',
    'resource.cpp',
  ),
  kwargs: lib_kwargs,
)
