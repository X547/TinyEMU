if fs_net_enabled
  executable('build_filelist',
    files('build_filelist.cpp'),
    fs_utils_src,
    cutils_src,
    cpp_args: common_args,
    include_directories: inc,
    dependencies: common_deps + [cc.find_library('m', required: false)],
    install: true,
  )

  executable('splitimg',
    files('splitimg.cpp'),
    cpp_args: common_args,
    include_directories: inc,
    dependencies: common_deps,
    install: true,
  )
endif
