/* riscv-tests "p" environment, retargeted at TinyEMU's HTIF registers.
 *
 * Upstream env/p/link.ld places .tohost in RAM at 0x80001000 and expects the
 * runner to find it by reading the ELF symbol table. TinyEMU deliberately does
 * not load ELF files; its HTIF is a device at the fixed address 0x40008000
 * (see readme.txt section 4.3), so the tests would otherwise write their result
 * into ordinary memory and spin forever.
 *
 * Placing the section at that address with (NOLOAD) moves the tohost and
 * fromhost symbols onto the device without giving the section any file
 * content, so "objcopy -O binary" still emits a compact image that starts at
 * 0x80000000 rather than spanning the gigabyte between the two addresses.
 */
OUTPUT_ARCH( "riscv" )
ENTRY(_start)

SECTIONS
{
  .tohost 0x40008000 (NOLOAD) : { *(.tohost) }

  . = 0x80000000;
  .text.init : { *(.text.init) }
  . = ALIGN(0x1000);
  .text : { *(.text) }
  . = ALIGN(0x1000);
  .data : { *(.data) }
  .bss : { *(.bss) }
  _end = .;
}
