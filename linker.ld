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
        *(.data.rel)
        *(.data.rel.*)
        *(.data.rel.ro)
        *(.data.rel.ro.*)

        /* GOT tables — needed by -fpie code */
        *(.got)
        *(.got.plt)

        /* ----------------------------------------------------------
         * v53: Persistent state section.
         *
         * Variables marked __attribute__((section(".ps_persist")))
         * land HERE — BEFORE __bss_start — so reset_doom_globals()'s
         * BSS wipe does NOT touch them.
         *
         * This is how g_wads[], g_wad_count, g_ctx, g_jmp_buf, etc.
         * survive between Doom sessions without any save/restore
         * dance.  See main.c and ps_libc.c for which variables use
         * PS_PERSIST.
         * ---------------------------------------------------------- */
        __ps_persist_start = .;
        *(.ps_persist)
        *(.ps_persist.*)
        __ps_persist_end = .;

        /* ----------------------------------------------------------
         * Doom's BSS.
         *
         * Every global that Doom allocates at file scope (lumps,
         * textures, level caches, zone, etc.) lives here.  Wiping
         * __bss_start..__bss_end between sessions resets all of
         * Doom's state so D_DoomMain can run a second time without
         * hitting "W_CacheLumpNum: N >= numlumps".
         *
         * Our own persistent state lives in .ps_persist ABOVE, so
         * this wipe doesn't touch it.
         * ---------------------------------------------------------- */
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :text

    /* ------------------------------------------------------------
     * .rela_out — holds the ELF relocation table.
     *
     * _start walks __rela_start..__rela_end and applies load_base
     * to every R_X86_64_RELATIVE entry.  That's how -fpie code gets
     * its absolute addresses resolved at our arbitrary mmap'd base.
     * ------------------------------------------------------------ */
    .rela_out : {
        __rela_start = .;
        *(.rela.dyn)
        *(.rela.dyn.*)
        *(.rela.text)
        *(.rela.text.*)
        *(.rela.data)
        *(.rela.data.*)
        *(.rela.data.rel)
        *(.rela.data.rel.*)
        *(.rela.rodata)
        *(.rela.rodata.*)
        *(.rela.bss)
        *(.rela.bss.*)
        *(.rela.got)
        *(.rela.got.*)
        *(.rela.init_array)
        *(.rela.fini_array)
        __rela_end = .;
    } :text

    /* ------------------------------------------------------------
     * Sections we discard because we're a freestanding single-segment
     * binary.  Anything the linker would normally emit for dynamic
     * linking, unwind tables, or PLT thunks is dropped.
     * ------------------------------------------------------------ */
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
