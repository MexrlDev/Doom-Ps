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

        /* BSS is kept INSIDE .text (not NOLOAD) so the file stays
         * around 628 KB — the console-side Lua launcher has a hard
         * minimum of ~623 KB and rejects anything smaller.
         * The bytes are zeros in the file, so globals start zeroed. */
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :text

    /* ------------------------------------------------------------------
     * .rela_out — holds the ELF relocation table emitted by -pie.
     *
     * At runtime, _start walks __rela_start..__rela_end and adds the
     * load base to every R_X86_64_RELATIVE entry.  This fixes absolute
     * pointers stored in .data (like `defaults[].location = &screenblocks`)
     * which the linker resolved to small offsets from 0.
     * ------------------------------------------------------------------ */
    .rela_out : {
        __rela_start = .;
        *(.rela.dyn)
        *(.rela.dyn.*)
        *(.rela.text)
        *(.rela.text.*)
        *(.rela.data)
        *(.rela.data.*)
        *(.rela.rodata)
        *(.rela.rodata.*)
        *(.rela.bss)
        *(.rela.bss.*)
        *(.rela.init_array)
        *(.rela.fini_array)
        *(.rela.plt)
        *(.rela.got)
        __rela_end = .;
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
        *(.plt)
        *(.plt.*)
    }
}
