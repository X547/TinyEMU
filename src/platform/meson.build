# Host integration: everything that talks to the operating system. The
# emulator libraries see only the interfaces in api/.

# The per host headers the common code includes, such as wait_set.h.
host_dir = platform_os == 'windows' ? 'win32' : 'posix'

platform_inc = [inc, include_directories('common', 'curl', 'haiku', host_dir,
                                         'sdl')]

platform_kwargs = lib_kwargs + {
  'include_directories': platform_inc,
}

# Services every build has, one implementation per host.
if platform_os == 'windows'
  host_sources = files('win32/host_memory.cpp', 'win32/host_time.cpp')
elif platform_os == 'haiku'
  host_sources = files('haiku/fs_disk_os.cpp', 'haiku/host_memory.cpp',
                       'posix/host_time.cpp')
else
  host_sources = files('posix/fs_disk_os.cpp', 'posix/host_memory.cpp',
                       'posix/host_time.cpp')
endif

emu_libs += static_library('platform_host', host_sources,
  kwargs: platform_kwargs)

# The event loop and the back ends the machine's devices connect to.
platform_sources = files(
  'common/console_escape.cpp',
  'common/event_loop.cpp',
  'common/file_block.cpp',
  'common/host_platform.cpp',
)
if platform_os == 'windows'
  platform_sources += files(
    'common/no_disk_fs.cpp',
    'win32/event_loop_wait.cpp',
    'win32/stdio_console.cpp',
  )
else
  platform_sources += files(
    'posix/event_loop_wait.cpp',
    'posix/fs_disk.cpp',
    'posix/stdio_console.cpp',
  )
endif
if platform_os == 'linux'
  platform_sources += files('linux/tap_ethernet.cpp')
else
  platform_sources += files('common/no_tap_ethernet.cpp')
endif
if slirp_enabled
  platform_sources += files('slirp/slirp_ethernet.cpp')
else
  platform_sources += files('common/no_slirp_ethernet.cpp')
endif
if not sdl_dep.found() and not haiku_gui and not win32_gui
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
if haiku_gui
  emu_libs += static_library('platform_haiku_gui',
    files('haiku/haiku_display.cpp', 'haiku/haiku_keymap.cpp'),
    kwargs: platform_kwargs,
  )
endif
if win32_gui
  emu_libs += static_library('platform_win32_gui',
    files('win32/win32_display.cpp', 'win32/win32_keymap.cpp'),
    kwargs: platform_kwargs,
  )
endif
if fs_net_enabled
  subdir('curl')
endif
