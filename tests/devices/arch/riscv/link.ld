/* Linked at 0 and relocated by entry.S to wherever it was loaded. The
   relocations are kept between the symbols entry.S walks; the stack follows
   the BSS, so nothing past __image_end is in use. */
ENTRY(_start)

SECTIONS
{
  . = 0;
  __image_start = .;
  .text : {
    KEEP(*(.text.entry))
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
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
  .data : { *(.data.rel.ro .data.rel.ro.* .data .data.* .sdata .sdata.*) }
  .got : { *(.got .got.*) }
  .dynamic : { *(.dynamic) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .hash : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  .rela.dyn : {
    __rela_start = .;
    *(.rela.*)
    __rela_end = .;
  }
  . = ALIGN(16);
  __bss_start = .;
  .bss : { *(.sbss .sbss.* .bss .bss.*) *(COMMON) }
  . = ALIGN(16);
  __bss_end = .;
  .stack (NOLOAD) : {
    . = ALIGN(4096);
    . += 0x10000;
    __stack_top = .;
  }
  __image_end = .;
  /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) *(.riscv.attributes) }
}
