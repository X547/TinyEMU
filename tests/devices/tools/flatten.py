#!/usr/bin/env python3
# flatten.py <readelf> <objcopy> <elf> <bin>: check that the image has only
# relative dynamic relocations, which the entry code applies, then write it
# as the flat binary temu loads.
import re
import subprocess
import sys

readelf, objcopy, elf, out = sys.argv[1:5]
relocs = subprocess.run([readelf, '-r', elf], check=True,
                        capture_output=True, text=True).stdout
bad = [line for line in relocs.splitlines()
       if re.search(r'R_RISCV_\w+', line) and 'R_RISCV_RELATIVE' not in line]
if bad:
    sys.stderr.write('relocations the entry code does not apply:\n')
    sys.stderr.write('\n'.join(bad[:20]) + '\n')
    sys.exit(1)
subprocess.run([objcopy, '-O', 'binary', elf, out], check=True)
