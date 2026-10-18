# The Wayland window. The protocols other than the core one are generated
# from their descriptions.

wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))
wayland_protocols_dir = dependency('wayland-protocols') \
  .get_variable('pkgdatadir')

wayland_protocols = [
  'stable/xdg-shell/xdg-shell.xml',
  'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
]
wayland_sources = files('be_decorator.cpp', 'title_font.cpp',
                        'wayland_display.cpp', 'window_buffer.cpp')
if wayland_dmabuf
  wayland_protocols += 'stable/linux-dmabuf/linux-dmabuf-v1.xml'
  wayland_sources += files('dmabuf_allocator.cpp')
endif

wayland_protocol_sources = []
foreach xml : wayland_protocols
  name = xml.split('/')[-1].replace('.xml', '')
  wayland_protocol_sources += custom_target(name + '-client-header',
    input: wayland_protocols_dir / xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  wayland_protocol_sources += custom_target(name + '-code',
    input: wayland_protocols_dir / xml,
    output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

emu_libs += static_library('platform_wayland_gui',
  wayland_sources,
  wayland_protocol_sources,
  kwargs: platform_kwargs,
)
