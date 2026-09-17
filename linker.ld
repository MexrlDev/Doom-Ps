OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = 0;

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
    } :text

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :text

    /* ------------------------------------------------------------------
     * Pad the output to a fixed 640 KB (0xA0000).
     *
     * Why: the Lua launcher on the console checks the received shellcode
     * size against an expected minimum (~623 KB) and rejects anything
     * smaller with "short receive: got N bytes, expected ~623 KB".
     *
     * Our actual code + rodata + data + bss is only ~354 KB, so we pad
     * the file with zeros up to 0xA0000.  The padding bytes sit AFTER
     * .bss in memory, so they don't affect execution — the entry point
     * still runs from offset 0 of the mmap, and BSS is still zeroed by
     * our _start handler (see main.c).
     *
     * If the launcher ever complains again about size, just change the
     * 0xA0000 below to whatever it wants — this is a pure file-size
     * knob, it has no effect on the running code.
     * ------------------------------------------------------------------ */
    .pad : {
        BYTE(0);
        . = 0xA0000;
    } :text

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.got)
        *(.got.plt)
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.gnu.hash)
        *(.gnu.version*)
        *(.rela.*)
        *(.rel.*)
        *(.plt)
        *(.plt.*)
    }
}
