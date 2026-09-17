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

        /* ------------------------------------------------------------
         * BSS is intentionally kept INSIDE .text (not NOLOAD).
         *
         * Why: the console-side Lua launcher on this firmware has a
         * hard minimum on received shellcode size (~623 KB).  With
         * BSS excluded from the file, our shellcode is only 354 KB
         * and gets rejected with:
         *
         *    "short receive: got 354400 bytes, expected ~623 KB"
         *
         * Keeping BSS inside .text gives a ~628 KB file that the
         * launcher accepts.  The BSS bytes are zeros in the file, so
         * globals are already zero-initialised before _start runs —
         * the explicit zeroing in main.c's _start becomes a harmless
         * no-op.
         *
         * Symbols __bss_start / __bss_end are still defined so
         * main.c's extern references resolve.
         * ------------------------------------------------------------ */
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
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
