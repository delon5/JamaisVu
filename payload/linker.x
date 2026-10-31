OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)

ENTRY(_start)

SECTIONS
{
  . = 0x1c000000;
  .text     : { *(.text.start) *(.text   .text.*   .gnu.linkonce.t.*) *(.sceStub.text.*) }
  .rodata   : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
  .data     : { *(.data   .data.*   .gnu.linkonce.d.*) }
  .bss      : {
    . = ALIGN(4);
    __bss_start = .;
    *(.bss    .bss.*    .gnu.linkonce.b.*) *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  }
  /DISCARD/ : { *(.interp) *(.dynsym) *(.dynstr) *(.hash) *(.dynamic) *(.comment) }
}

/* The payload (including .bss) must end before the arguments block at
 * PAYLOAD_ARGS_PADDR (see shared.h). */
ASSERT(__bss_end <= 0x1c080000, "payload overlaps PAYLOAD_ARGS_PADDR")
