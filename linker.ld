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

    /* ------------------------------------------------------------------
     * .bss marked (NOLOAD) so it occupies memory at runtime but does NOT
     * bloat the on-disk file.  Luac0re's loader memcpy's the file bytes
     * into mmap'd memory, then our _start prologue zeroes __bss_start ..
     * __bss_end (see main.c).  Without (NOLOAD) the linker writes ~600 KB
     * of zeros to the file, blowing past the 1 MB mmap and causing the
     * "Broken pipe" + stuck LOADING screen.
     * ------------------------------------------------------------------ */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :text

    /* ------------------------------------------------------------------
     * Pad the output file to exactly 640 KB (0xA0000).
     *
     * Why: the Lua launcher on the console rejects anything smaller than
     * ~623 KB ("short receive: got N bytes, expected ~623 KB").  640 KB
     * is safely above that and well under the 1 MB mmap.
     *
     * If the launcher's required size ever changes, edit the 0xA0000
     * below.  This is a pure file-size knob; it has zero effect on the
     * running code.
     * ------------------------------------------------------------------ */
    .pad : {
        FILL(0);
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
