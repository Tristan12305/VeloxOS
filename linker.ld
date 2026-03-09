OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)

KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x0000000000200000;
KERNEL_OFFSET = KERNEL_VMA - KERNEL_LMA;

PHDRS
{
    text PT_LOAD FLAGS((1 << 0) | (1 << 2)); /* R-X */
    rodata PT_LOAD FLAGS((1 << 2));           /* R-- */
    data PT_LOAD FLAGS((1 << 1) | (1 << 2));  /* RW- */
}

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_virtual_start = .;

    . = ALIGN(4K);
    .text : AT(ADDR(.text) - KERNEL_OFFSET)
    {
        *(.text .text.*)
    } :text

    . = ALIGN(4K);
    .rodata : AT(ADDR(.rodata) - KERNEL_OFFSET)
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(8);
    .limine_requests : AT(ADDR(.limine_requests) - KERNEL_OFFSET)
    {
        KEEP(*(.requests_start_marker .requests_start_marker.*))
        KEEP(*(.requests .requests.*))
        KEEP(*(.requests_end_marker .requests_end_marker.*))
    } :rodata

    . = ALIGN(4K);
    .data : AT(ADDR(.data) - KERNEL_OFFSET)
    {
        *(.data .data.*)
    } :data

    . = ALIGN(4K);
    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_OFFSET)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4K);
    __kernel_virtual_end = .;
    __kernel_physical_start = __kernel_virtual_start - KERNEL_OFFSET;
    __kernel_physical_end = __kernel_virtual_end - KERNEL_OFFSET;

    /DISCARD/ :
    {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
