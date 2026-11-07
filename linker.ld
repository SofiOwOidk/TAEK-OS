OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(principal)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Ejecutar + Leer */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Solo lectura */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Escribir + Leer */
    tls     PT_TLS     FLAGS((1 << 2));
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        KEEP(*(.requests_start_marker))
        KEEP(*(.requests))
        KEEP(*(.requests_end_marker))
    } :data

    /* No permitir secciones huérfanas entre .tdata y .tbss: PT_TLS debe
       describir los bytes iniciales y toda la reserva cero sin solaparse. */
    .got : { *(.got .got.*) } :data
    .tdata : ALIGN(64) {
        __taek_tls_inicio = .;
        *(.tdata .tdata.*)
        __taek_tls_datos_fin = .;
    } :data :tls
    .tbss : ALIGN(64) {
        *(.tbss .tbss.*)
        __taek_tls_fin = .;
    } :data :tls
    __taek_tls_alineacion = MAX(64, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
    . = ALIGN(ADDR(.tbss) + SIZEOF(.tbss), 64);

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
