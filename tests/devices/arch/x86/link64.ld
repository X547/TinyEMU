/* Loaded by the PVH loader at the physical addresses of the PT_LOAD
   segments, from 1 MB, with the entry point from the Xen PHYS32_ENTRY
   note. The page tables lead the BSS, which is page aligned. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(pvh_entry)

SECTIONS
{
  . = 0x100000;
  __image_start = .;
  .note.Xen : { *(.note.Xen) }
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  }
  devtests : {
    __devtests_start = .;
    KEEP(*(devtests))
    __devtests_end = .;
  }
  .data : { *(.data .data.*) }
  . = ALIGN(4096);
  __bss_start = .;
  .bss : { *(.bss.pagetables) *(.bss .bss.*) *(COMMON) }
  . = ALIGN(16);
  __bss_end = .;
  .stack (NOLOAD) : {
    . = ALIGN(4096);
    . += 0x10000;
    __stack_top = .;
  }
  __image_end = .;
  /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.eh_frame*) }
}
