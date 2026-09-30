/* ICI benchmark: loaded by temu's PVH loader, which takes the PT_LOAD
 * segments at their physical addresses (1 MB and up) and the entry point
 * from the Xen PHYS32_ENTRY note. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(pvh_entry)

SECTIONS
{
  . = 0x100000;
  .note.Xen : { *(.note.Xen) }
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.eh_frame) }
}
