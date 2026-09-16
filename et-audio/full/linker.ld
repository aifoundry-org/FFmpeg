/* Full AAC requires FFmpeg mutable tables. The old loader does not zero LOAD
 * tails, so BSS is deliberately emitted as explicit PROGBITS zero bytes. */
SECTIONS {
 . = BASE_ADDRESS; KERNEL_UMODE_ENTRY = BASE_ADDRESS;
 .text.init : { KEEP(*(.text.init)) }
 . = ALIGN(64); .text : { *(.text .text.*) } =0x13000000
 . = ALIGN(64); .rodata : { *(.rodata .rodata.* .srodata .srodata.*) . = ALIGN(64); }
 . = ALIGN(64); .data : {
   __aac_globals_start = .;
   BYTE(0); *(.sdata .sdata.*) PROVIDE(__global_pointer$ = . + 0x800);
   *(.data .data.*) *(.sbss .sbss.* .bss .bss.* COMMON) . = ALIGN(64);
   __aac_globals_end = .;
 }
 .tdata : { *(.tdata .tdata.*) }
 .tbss (NOLOAD) : { *(.tbss .tbss.*) }
 ASSERT(SIZEOF(.tdata)==0 && SIZEOF(.tbss)==0, "AAC kernel must not contain TLS")
 . = ALIGN(64); _end = .;
}
