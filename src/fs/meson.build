# Shared with build_filelist, which cannot name a file outside its directory.
fs_utils_src = files('fs_utils.cpp')

fs_sources = files(
  'fs.cpp',
  'fs_disk.cpp',
)
if fs_net_enabled
  fs_sources += fs_utils_src
  fs_sources += files(
    'block_net.cpp',
    'fs_net.cpp',
    'fs_wget.cpp',
  )
endif

emu_libs += static_library('fs', fs_sources, kwargs: lib_kwargs)
