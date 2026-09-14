# HTTP back ends: disk images and file systems fetched with libcurl.

# Shared with build_filelist, which cannot name a file outside its directory.
fs_utils_src = files('fs_utils.cpp')

emu_libs += static_library('platform_curl',
  fs_utils_src,
  files(
    'block_net.cpp',
    'fs.cpp',
    'fs_net.cpp',
    'fs_wget.cpp',
    'url_backends.cpp',
  ),
  kwargs: platform_kwargs,
)
