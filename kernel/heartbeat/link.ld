ENTRY(_start)
SECTIONS {
    . = 0;
    .text   : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    /* .bss folded into .data: nothing zeroes RAM before _start, so zeros must ship in the image */
    .data   : { *(.data*) *(.bss*) *(COMMON) }
    _image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
