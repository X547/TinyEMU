#
# TinyEMU
#
# Copyright (c) 2016-2018 Fabrice Bellard
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#

project('tinyemu', ['c', 'cpp'],
  version: files('VERSION'),
  license: 'MIT',
  default_options: [
    'c_std=gnu99',
    'cpp_std=gnu++20',
    'warning_level=1',
    'buildtype=debugoptimized',
  ],
  meson_version: '>= 1.1.0',
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

common_defines = [
  '-D_FILE_OFFSET_BITS=64',
  '-D_LARGEFILE_SOURCE',
  '-D_GNU_SOURCE',
  '-DCONFIG_VERSION="' + meson.project_version() + '"',
]

# Silence the warnings the upstream sources trip over so that real problems
# stay visible. The emulator is C++ and slirp stays C, so each compiler gets its
# own probed list; common_defines keeps accumulating below and the two argument
# lists are assembled at the point of use.
common_warn_args = [
  '-Wno-unused-but-set-variable',
  '-Wno-format-truncation',
  '-Wno-stringop-truncation',
]

c_warn_args = cc.get_supported_arguments(common_warn_args)

# C++ turns on -Wsign-compare as part of -Wall, which lights up ~35 upstream
# comparisons that the C build never diagnosed. They predate the port, so they
# are silenced here rather than left to drown out new problems.
cpp_warn_args = cpp.get_supported_arguments(common_warn_args + [
  '-Wno-class-memaccess',
  '-Wno-sign-compare',
])

# Dependencies needed by every target.
common_deps = []

# Haiku (and some other systems) keep the socket API in a separate library.
foreach libname : ['network', 'socket']
  lib = cc.find_library(libname, required: false)
  if lib.found()
    common_deps += lib
  endif
endforeach

# Dependencies needed by the emulator only.
emu_deps = []

# ----------------------------------------------------------------------------
# options

max_xlen = get_option('int128') ? 128 : 64
if max_xlen == 128 and not cc.has_type('__int128')
  error('the int128 option requires a compiler supporting __int128')
endif
common_defines += '-DCONFIG_RISCV_MAX_XLEN=@0@'.format(max_xlen)

x86emu = get_option('x86emu')
if x86emu
  common_defines += '-DCONFIG_X86EMU'
endif

sdl_dep = dependency('sdl', required: get_option('sdl'))
if sdl_dep.found()
  common_defines += '-DCONFIG_SDL'
  emu_deps += sdl_dep
endif

fs_net = get_option('fs_net')
builtin_crypto = get_option('builtin_crypto')
curl_dep = dependency('libcurl', required: fs_net)

crypto_dep = dependency('', required: false)
if curl_dep.found() and not builtin_crypto
  crypto_dep = dependency('libcrypto', required: fs_net)
endif

fs_net_enabled = curl_dep.found() and (builtin_crypto or crypto_dep.found())
if fs_net_enabled
  common_defines += '-DCONFIG_FS_NET'
  emu_deps += curl_dep
  if builtin_crypto
    common_defines += '-DUSE_BUILTIN_CRYPTO'
  else
    emu_deps += crypto_dep
  endif
endif

# ----------------------------------------------------------------------------
# slirp

if get_option('slirp')
  common_defines += '-DCONFIG_SLIRP'
  common_c_args = common_defines + c_warn_args
  subdir('slirp')
  emu_deps += slirp_dep
endif

common_args = common_defines + cpp_warn_args

# ----------------------------------------------------------------------------
# sources

emu_sources = files(
  'cutils.cpp',
  'fs.cpp',
  'fs_disk.cpp',
  'iomem.cpp',
  'json.cpp',
  'machine.cpp',
  'pci.cpp',
  'riscv_machine.cpp',
  'simplefb.cpp',
  'softfp.cpp',
  'temu.cpp',
  'uart.cpp',
  'virtio.cpp',
  'virtio_9p.cpp',
  'virtio_block.cpp',
  'virtio_console.cpp',
  'virtio_input.cpp',
  'virtio_net.cpp',
)

if x86emu
  emu_sources += files(
    'ide.cpp',
    'pckbd.cpp',
    'ps2.cpp',
    'vga.cpp',
    'vmmouse.cpp',
    'x86_cpu.cpp',
    'x86_machine.cpp',
  )
endif

if sdl_dep.found()
  emu_sources += files('sdl.cpp')
endif

if fs_net_enabled
  emu_sources += files(
    'block_net.cpp',
    'fs_net.cpp',
    'fs_utils.cpp',
    'fs_wget.cpp',
  )
  if builtin_crypto
    emu_sources += files('aes.cpp', 'sha256.cpp')
  endif
endif

# riscv_cpu.cpp is compiled once per supported XLEN.
riscv_cpu_libs = []
xlens = [32, 64]
if max_xlen == 128
  xlens += 128
endif

foreach xlen : xlens
  riscv_cpu_libs += static_library('riscv_cpu@0@'.format(xlen),
    files('riscv_cpu.cpp'),
    cpp_args: common_args + ['-DMAX_XLEN=@0@'.format(xlen)],
    dependencies: common_deps,
  )
endforeach

# ----------------------------------------------------------------------------
# targets

executable('temu',
  emu_sources,
  cpp_args: common_args,
  link_with: riscv_cpu_libs,
  dependencies: common_deps + emu_deps,
  install: true,
)

if fs_net_enabled
  executable('build_filelist',
    files('build_filelist.cpp', 'fs_utils.cpp', 'cutils.cpp'),
    cpp_args: common_args,
    dependencies: common_deps + [cc.find_library('m', required: false)],
    install: true,
  )

  executable('splitimg',
    files('splitimg.cpp'),
    cpp_args: common_args,
    dependencies: common_deps,
    install: true,
  )
endif

summary({
  'SDL display': sdl_dep.found(),
  'x86 emulator': x86emu,
  'slirp': get_option('slirp'),
  'network filesystem': fs_net_enabled,
  'builtin crypto': fs_net_enabled and builtin_crypto,
  'RISCV max XLEN': max_xlen,
}, section: 'Configuration', bool_yn: true)
