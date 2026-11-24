# tvmm: TinyEMU's hypervisor kernel add-on for Haiku. Built on Haiku, against
# the private kernel headers of the running system.

project('tvmm', 'cpp', 'c',
	version: '0.1',
	# install_symlink()
	meson_version: '>=0.61',
	default_options: [
		# the run loop's cost is what is measured
		'buildtype=debugoptimized',
		'cpp_std=c++20',
		'warning_level=2',
		'werror=true',
	]
)

cpp = meson.get_compiler('cpp')

kernel_headers = '/boot/system/develop/headers/private'

kernel_cpp_args = [
	'-D_KERNEL_MODE',
	'-fno-exceptions',
	'-fno-rtti',
	'-fno-omit-frame-pointer',
	'-Wno-missing-field-initializers',
]

cpu_family = host_machine.cpu_family()
if cpu_family == 'x86_64'
	kernel_cpp_args += ['-mno-red-zone']
	kernel_arch = 'x86'
	kernel_system_arch = 'x86_64'
	kernel_platform = 'bios_ia32'
elif cpu_family == 'riscv64'
	# as Haiku builds the kernel and its add-ons
	kernel_cpp_args += ['-mcmodel=medany']
	kernel_arch = 'riscv64'
	kernel_system_arch = 'riscv64'
	kernel_platform = 'efi'
else
	error('tvmm has no back end for ' + cpu_family)
endif

# The private kernel headers expect the build configuration headers of the
# Haiku source tree the kernel was built from, copied to "config_headers".
kernel_inc = include_directories(
	'config_headers',
	kernel_headers / 'kernel',
	kernel_headers / 'kernel/arch' / kernel_arch,
	kernel_headers / 'kernel/boot/platform' / kernel_platform,
	kernel_headers / 'system',
	kernel_headers / 'system/arch' / kernel_system_arch,
	kernel_headers / 'shared',
	kernel_headers,
	is_system: true,
)

kernel_link_args = [
	'/boot/system/develop/lib/_KERNEL_',
	run_command(cpp.cmd_array(), '-print-file-name=crtbegin.o', check: true).stdout().strip(),
	run_command(cpp.cmd_array(), '-print-file-name=crtend.o', check: true).stdout().strip(),
	'/boot/system/develop/lib/haiku_version_glue.o',
	'-nostdlib',
	'-Wl,--no-undefined',
	'-Wl,-z,max-page-size=0x1000',
]

tvmm_inc = include_directories('include', 'core', 'wrappers')

if cpu_family == 'x86_64'
	arch_inc = include_directories('arch/x86')
	arch_sources = files(
		'arch/x86/IoApic.cpp',
		'arch/x86/LocalApic.cpp',
		'arch/x86/Npt.cpp',
		'arch/x86/SvmEntry.S',
		'arch/x86/SvmHost.cpp',
		'arch/x86/SvmVcpu.cpp',
		'arch/x86/SvmVm.cpp',
		'arch/x86/X86Decoder.cpp',
	)
	tests = ['tvmm_smoke', 'tvmm_stress']
else
	arch_inc = include_directories('arch/riscv')
	arch_sources = files(
		'arch/riscv/GStage.cpp',
		'arch/riscv/RiscVDecoder.cpp',
		'arch/riscv/RiscVEntry.S',
		'arch/riscv/RiscVHost.cpp',
		'arch/riscv/RiscVVcpu.cpp',
		'arch/riscv/RiscVVm.cpp',
	)
	tests = ['tvmm_riscv_smoke']
endif

# `meson install` puts the driver among the add-ons of the prefix, which is
# one of Haiku's directories that have them: /boot/system or ~/config, or
# non-packaged in either. It goes in as add-ons/kernel/drivers/bin/tvmm, with
# the link devfs publishes it by, add-ons/kernel/drivers/dev/misc/tvmm.
#
# Nothing may use the device while it installs, nor until devfs has settled
# afterwards; see README.md. devfs takes a changed binary of a loaded driver
# later, from a kernel daemon, and the link is made again at each install,
# which drops the driver and adds it back, also later. An open of the device
# meanwhile can panic the system.
drivers_dir = 'add-ons/kernel/drivers'

shared_module('tvmm',
	'core/Driver.cpp',
	'core/GuestMemory.cpp',
	'core/KernelArea.cpp',
	'core/Vcpu.cpp',
	'core/Vm.cpp',
	'core/VmHandle.cpp',
	arch_sources,
	# a driver's binary has no prefix or suffix
	name_prefix: '',
	name_suffix: '',
	include_directories: [tvmm_inc, arch_inc, kernel_inc],
	cpp_args: kernel_cpp_args,
	c_args: ['-D_KERNEL_MODE'] + (cpu_family == 'riscv64' ? ['-mcmodel=medany'] : []),
	link_args: kernel_link_args,
	install: true,
	install_dir: drivers_dir / 'bin',
)

install_symlink('tvmm',
	pointing_to: '../../bin/tvmm',
	install_dir: drivers_dir / 'dev/misc',
)

foreach test: tests
	executable(test,
		'tests' / test + '.cpp',
		include_directories: [
			include_directories('include'),
			include_directories(kernel_headers / 'shared', is_system: true),
		],
	)
endforeach
