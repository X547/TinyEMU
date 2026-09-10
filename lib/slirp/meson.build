# User space network redirector (slirp), built as a helper library for temu.

slirp_lib = static_library('slirp',
  files(
    'bootp.c',
    'cksum.c',
    'if.c',
    'ip_icmp.c',
    'ip_input.c',
    'ip_output.c',
    'mbuf.c',
    'misc.c',
    'sbuf.c',
    'slirp.c',
    'socket.c',
    'tcp_input.c',
    'tcp_output.c',
    'tcp_subr.c',
    'tcp_timer.c',
    'udp.c',
  ),
  c_args: common_c_args,
  include_directories: inc,
  dependencies: common_deps,
)

slirp_dep = declare_dependency(
  link_with: slirp_lib,
  include_directories: include_directories('.'),
)
