# tvmm: TinyEMU's hypervisor kernel add-on for Haiku. Built on Haiku, against
# the private kernel headers of the running system.

project('tvmm', 'cpp', 'c',
	version: '0.1',
	default_options: [
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
	'-mno-red-zone',
	'-fno-omit-frame-pointer',
	'-Wno-missing-field-initializers',
]

# The private kernel headers expect the build configuration headers of the
# Haiku source tree the kernel was built from, copied to "config_headers".
kernel_inc = include_directories(
	'config_headers',
	kernel_headers / 'kernel',
	kernel_headers / 'kernel/arch/x86',
	kernel_headers / 'kernel/boot/platform/bios_ia32',
	kernel_headers / 'system',
	kernel_headers / 'system/arch/x86_64',
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

arch_sources = []
if host_machine.cpu_family() == 'x86_64'
	arch_inc = include_directories('arch/x86')
	arch_sources = files(
		'arch/x86/LocalApic.cpp',
		'arch/x86/Npt.cpp',
		'arch/x86/SvmEntry.S',
		'arch/x86/SvmHost.cpp',
		'arch/x86/SvmVcpu.cpp',
		'arch/x86/SvmVm.cpp',
		'arch/x86/X86Decoder.cpp',
	)
else
	error('tvmm has no back end for ' + host_machine.cpu_family())
endif

shared_module('tvmm',
	'core/Driver.cpp',
	'core/GuestMemory.cpp',
	'core/KernelArea.cpp',
	'core/Vcpu.cpp',
	'core/Vm.cpp',
	'core/VmHandle.cpp',
	arch_sources,
	name_prefix: '',
	include_directories: [tvmm_inc, arch_inc, kernel_inc],
	cpp_args: kernel_cpp_args,
	c_args: ['-D_KERNEL_MODE'],
	link_args: kernel_link_args,
)

executable('tvmm_smoke',
	'tests/tvmm_smoke.cpp',
	include_directories: [
		include_directories('include'),
		include_directories(kernel_headers / 'shared', is_system: true),
	],
)
