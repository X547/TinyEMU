# Host integration: everything that talks to the operating system. The
# emulator libraries see only the interfaces in api/.

platform_inc = [inc, include_directories('common', 'curl', 'posix', 'sdl')]

platform_kwargs = lib_kwargs + {
  'include_directories': platform_inc,
}

# Services every build has, one implementation per host.
host_sources = files('posix/host_time.cpp')
if platform_os == 'haiku'
  host_sources += files('haiku/fs_disk_os.cpp', 'haiku/host_memory.cpp')
else
  host_sources += files('posix/fs_disk_os.cpp', 'posix/host_memory.cpp')
endif

emu_libs += static_library('platform_host', host_sources,
  kwargs: platform_kwargs)

# The event loop and the back ends the machine's devices connect to.
platform_sources = files(
  'common/event_loop.cpp',
  'common/host_platform.cpp',
  'posix/event_loop_wait.cpp',
  'posix/file_block.cpp',
  'posix/fs_disk.cpp',
  'posix/stdio_console.cpp',
)
if platform_os == 'linux'
  platform_sources += files('linux/tap_ethernet.cpp')
else
  platform_sources += files('common/no_tap_ethernet.cpp')
endif
if get_option('slirp')
  platform_sources += files('slirp/slirp_ethernet.cpp')
else
  platform_sources += files('common/no_slirp_ethernet.cpp')
endif
if not sdl_dep.found()
  platform_sources += files('common/no_display.cpp')
endif
if not fs_net_enabled
  platform_sources += files('common/no_url_backends.cpp')
endif

emu_libs += static_library('platform', platform_sources,
  kwargs: platform_kwargs)

if sdl_dep.found()
  subdir('sdl')
endif
if fs_net_enabled
  subdir('curl')
endif
