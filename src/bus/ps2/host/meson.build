ps2_host_sources = []

# The i8042 is a PC part: it is the one controller here reached through port
# I/O, and the only one with an AT scancode translator.
if x86emu
  ps2_host_sources += files('i8042.cpp')
endif
