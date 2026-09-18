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

        /* GOT tables — needed by -fpie code */
        *(.got)
        *(.got.plt)

        /* ----------------------------------------------------------
         * v53: Persistent state section.  NEVER wiped by
         * reset_doom_globals().  Any global we need to survive between
         * Doom sessions (function pointers, WAD list, memory pool
         * pointer) is marked __attribute__((section(".ps_persist"))).
         * ---------------------------------------------------------- */
        __ps_persist_start = .;
        *(.ps_persist)
        *(.ps_persist.*)
        __ps_persist_end = .;

        /* ----------------------------------------------------------
         * Doom's BSS.  Wiped between sessions so D_DoomMain can run
         * a second time without stale lumps / textures / level data.
         * ---------------------------------------------------------- */
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :text

    /* .rela_out — ELF relocation table.  _start walks __rela_start..
     * __rela_end and applies load_base to every R_X86_64_RELATIVE. */
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
        *(.rela.got)
        *(.rela.got.*)
        __rela_end = .;
    } :text

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame.*)
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.dynamic)
        *(.gnu.hash)
        *(.gnu.version)
        *(.gnu.version_d)
        *(.gnu.version_r)
        *(.plt)
        *(.plt.*)
    }
}
