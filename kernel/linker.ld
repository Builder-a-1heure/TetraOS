OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(start)

MEMORY {
    ram (rwx) : ORIGIN = 0x00010000, LENGTH = 64M
}

SECTIONS {
    . = 0x00010000;

    .magic : {
        LONG(0xDEADBEEF)    /* signature cherchée par stage2 */
    } >ram

    .text.start : {
        *(.text.start)
    } >ram

    .text : {
        *(.text)
    } >ram

    .rodata : {
        *(.rodata*)
    } >ram

    .data : { *(.data) } >ram

    .bss : {
        _bss_start = .;
        *(.bss)
        *(COMMON)
        _bss_end = .;
    } >ram

    _kernel_start = ADDR(.text.start);
    _kernel_end = _bss_end;

    /DISCARD/ : {
        *(.comment) *(.note) *(.eh_frame)
    }
}
