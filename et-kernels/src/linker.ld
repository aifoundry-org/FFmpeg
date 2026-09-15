/* Standalone GP-SDK kernel layout, adapted from ggml-et's bare-metal linker.
 * Runtime initializes SP; there is deliberately NO bss initialization. */
SECTIONS {
    . = BASE_ADDRESS;
    KERNEL_UMODE_ENTRY = BASE_ADDRESS;
    .text.init : { KEEP(*(.text.init)) }
    . = ALIGN(64);
    .text : { *(.text .text.*) }
    . = ALIGN(64);
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) . = ALIGN(64); }
    . = ALIGN(64);
    .data : {
        *(.sdata .sdata.*)
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.data .data.*)
        . = ALIGN(64);
    }
    .bss (NOLOAD) : { *(.sbss .sbss.* .bss .bss.* COMMON) }
    .tdata : { *(.tdata .tdata.*) }
    .tbss (NOLOAD) : { *(.tbss .tbss.*) }
    ASSERT(SIZEOF(.bss) == 0, "ET kernel must not contain .bss")
    ASSERT(SIZEOF(.tdata) == 0 && SIZEOF(.tbss) == 0, "ET kernel must not contain TLS")
    . = ALIGN(64);
    _end = .;
}
