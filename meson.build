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

# The host operating system's platform code. OS specific parts are chosen by
# picking source files, not by testing the OS name in the sources.
platform_os = host_machine.system()

# ----------------------------------------------------------------------------
# include path
#
# Includes stay unqualified ("pci.h", not "bus/pci/pci.h"), so every directory
# holding a header is on the search path. The emulated hardware only sees the
# platform interfaces in src/platform/api; the implementations add their own
# directories through platform_inc.

# The configuration loader still fetches files over HTTP itself.
curl_inc = include_directories('src/platform/curl')

inc = include_directories(
  'lib',
  'lib/crypto',
  'lib/softfp',
  'src/bus/ata',
  'src/bus/ata/host',
  'src/bus/hid',
  'src/bus/mdio',
  'src/bus/pci',
  'src/bus/pci/host',
  'src/bus/ps2',
  'src/bus/ps2/host',
  'src/bus/scsi',
  'src/bus/sd',
  'src/bus/sd/host',
  'src/bus/usb',
  'src/bus/usb/host',
  'src/bus/virtio',
  'src/core',
  'src/cpu/riscv',
  'src/cpu/x86',
  'src/device/char',
  'src/device/display',
  'src/device/fs',
  'src/device/input',
  'src/device/intc',
  'src/device/net',
  'src/device/power',
  'src/device/storage',
  'src/machine',
  'src/platform/api',
)

# ----------------------------------------------------------------------------
# slirp
#
# Comes before common_args is frozen below, because it is the last thing to add
# to common_defines.

if get_option('slirp')
  common_defines += '-DCONFIG_SLIRP'
  common_c_args = common_defines + c_warn_args
  subdir('lib/slirp')
  emu_deps += slirp_dep
endif

common_args = common_defines + cpp_warn_args

# Every emulator library is built the same way; each subdirectory appends its
# own to emu_libs.
lib_kwargs = {
  'cpp_args': common_args,
  'include_directories': inc,
  'dependencies': common_deps + emu_deps,
  'install': false,
}

emu_libs = []

# ----------------------------------------------------------------------------
# sources

subdir('lib/softfp')
subdir('lib/crypto')

subdir('src/core')
subdir('src/cpu/riscv')
subdir('src/cpu/x86')
subdir('src/bus/ata')
subdir('src/bus/hid')
subdir('src/bus/mdio')
subdir('src/bus/pci')
subdir('src/bus/ps2')
subdir('src/bus/scsi')
subdir('src/bus/sd')
subdir('src/bus/usb')
subdir('src/bus/virtio')
subdir('src/device/char')
subdir('src/device/display')
subdir('src/device/fs')
subdir('src/device/input')
subdir('src/device/intc')
subdir('src/device/net')
subdir('src/device/power')
subdir('src/device/storage')
subdir('src/machine')
subdir('src/platform')
subdir('src/main')

# After src/core and src/platform, which export the sources it shares with
# temu.
subdir('tools')

# ----------------------------------------------------------------------------
# targets
#
# link_whole, not link_with: these libraries reference each other both ways --
# machine/devices.cpp constructs every device, while the host bridges call back
# into machine -- and a cycle between static archives does not resolve in a
# single left to right link pass. Pulling every object in makes the order
# irrelevant, and costs nothing when the executable links all of it anyway.

executable('temu',
  link_whole: emu_libs,
  link_with: riscv_cpu_libs,
  cpp_args: common_args,
  dependencies: common_deps + emu_deps,
  install: true,
)

summary({
  'SDL display': sdl_dep.found(),
  'x86 emulator': x86emu,
  'slirp': get_option('slirp'),
  'network filesystem': fs_net_enabled,
  'builtin crypto': fs_net_enabled and builtin_crypto,
  'RISCV max XLEN': max_xlen,
}, section: 'Configuration', bool_yn: true)
