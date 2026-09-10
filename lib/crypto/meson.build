if fs_net_enabled and builtin_crypto
  emu_libs += static_library('crypto',
    files(
      'aes.cpp',
      'sha256.cpp',
    ),
    kwargs: lib_kwargs,
  )
endif
