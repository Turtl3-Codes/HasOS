ENTRY(kernel_main)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* Read + Execute */
    rodata PT_LOAD FLAGS(4); /* Read-only */
    data PT_LOAD FLAGS(6);   /* Read + Write */
}

SECTIONS
{
    . = 0xFFFFFFFF80000000;

    .text : {
        *(.text*)
    } :text

    . += CONSTANT(MAXPAGESIZE);

    .rodata : {
        *(.rodata*)
    } :rodata

    . += CONSTANT(MAXPAGESIZE);

    .limine_requests : ALIGN(8)
    {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests*))
        KEEP(*(.limine_requests_end))
    } :data

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data
}
