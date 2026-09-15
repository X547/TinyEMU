# The key table depends on the window system SDL runs on.
if platform_os == 'haiku'
  sdl_keymap = files('keymap_haiku.cpp', '../haiku/haiku_keymap.cpp')
else
  sdl_keymap = files('keymap_xorg.cpp')
endif

emu_libs += static_library('platform_sdl',
  files('sdl_display.cpp'),
  sdl_keymap,
  kwargs: platform_kwargs,
)
