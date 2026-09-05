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

project('tinyemu', 'c',
  version: files('VERSION'),
  license: 'MIT',
  default_options: [
    'c_std=gnu99',
    'warning_level=1',
    'buildtype=debugoptimized',
  ],
  meson_version: '>= 1.1.0',
)

cc = meson.get_compiler('c')

common_args = [
  '-D_FILE_OFFSET_BITS=64',
  '-D_LARGEFILE_SOURCE',
  '-D_GNU_SOURCE',
  '-DCONFIG_VERSION="' + meson.project_version() + '"',
]

# Silence the warnings the upstream sources trip over so that real problems
# stay visible.
common_args += cc.get_supported_arguments([
  '-Wno-unused-but-set-variable',
  '-Wno-format-truncation',
  '-Wno-stringop-truncation',
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
common_args += '-DCONFIG_RISCV_MAX_XLEN=@0@'.format(max_xlen)

x86emu = get_option('x86emu')
if x86emu
  common_args += '-DCONFIG_X86EMU'
endif

sdl_dep = dependency('sdl', required: get_option('sdl'))
if sdl_dep.found()
  common_args += '-DCONFIG_SDL'
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
  common_args += '-DCONFIG_FS_NET'
  emu_deps += curl_dep
  if builtin_crypto
    common_args += '-DUSE_BUILTIN_CRYPTO'
  else
    emu_deps += crypto_dep
  endif
endif

# ----------------------------------------------------------------------------
# slirp

if get_option('slirp')
  common_args += '-DCONFIG_SLIRP'
  subdir('slirp')
  emu_deps += slirp_dep
endif

# ----------------------------------------------------------------------------
# sources

emu_sources = files(
  'cutils.c',
  'fs.c',
  'fs_disk.c',
  'iomem.c',
  'json.c',
  'machine.c',
  'pci.c',
  'riscv_machine.c',
  'simplefb.c',
  'softfp.c',
  'temu.c',
  'uart.c',
  'virtio.c',
)

if x86emu
  emu_sources += files(
    'ide.c',
    'pckbd.c',
    'ps2.c',
    'vga.c',
    'vmmouse.c',
    'x86_cpu.c',
    'x86_machine.c',
  )
endif

if sdl_dep.found()
  emu_sources += files('sdl.c')
endif

if fs_net_enabled
  emu_sources += files(
    'block_net.c',
    'fs_net.c',
    'fs_utils.c',
    'fs_wget.c',
  )
  if builtin_crypto
    emu_sources += files('aes.c', 'sha256.c')
  endif
endif

# riscv_cpu.c is compiled once per supported XLEN.
riscv_cpu_libs = []
xlens = [32, 64]
if max_xlen == 128
  xlens += 128
endif

foreach xlen : xlens
  riscv_cpu_libs += static_library('riscv_cpu@0@'.format(xlen),
    files('riscv_cpu.c'),
    c_args: common_args + ['-DMAX_XLEN=@0@'.format(xlen)],
    dependencies: common_deps,
  )
endforeach

# ----------------------------------------------------------------------------
# targets

executable('temu',
  emu_sources,
  c_args: common_args,
  link_with: riscv_cpu_libs,
  dependencies: common_deps + emu_deps,
  install: true,
)

if fs_net_enabled
  executable('build_filelist',
    files('build_filelist.c', 'fs_utils.c', 'cutils.c'),
    c_args: common_args,
    dependencies: common_deps + [cc.find_library('m', required: false)],
    install: true,
  )

  executable('splitimg',
    files('splitimg.c'),
    c_args: common_args,
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
