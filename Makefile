# ============================================================
# doom-ps/Makefile
#
# v19: -pie for relocations; $(shell ls|grep) for source discovery
# (proven on this CI); objcopy dumps whole segment (not just .text).
# ============================================================

DOOM_DIR := doomgeneric

CC      := gcc
OBJCOPY := objcopy

# ------------------------------------------------------------------
# C flags
# ------------------------------------------------------------------
CFLAGS := \
    -Os \
    -ffreestanding \
    -fno-stack-protector \
    -fno-builtin \
    -fpie \
    -mno-red-zone \
    -fomit-frame-pointer \
    -fcf-protection=none \
    -fno-exceptions \
    -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -Wall -Wno-unused-function -Wno-unused-variable \
    -Wno-unused-but-set-variable \
    -Isrc \
    -I$(DOOM_DIR)

# ------------------------------------------------------------------
# Linker flags — -pie is REQUIRED so the linker emits .rela.dyn
# ------------------------------------------------------------------
LDFLAGS := \
    -T linker.ld \
    -nostdlib -nostartfiles \
    -Wl,--build-id=none \
    -Wl,--no-dynamic-linker \
    -Wl,-z,norelro \
    -pie

# ------------------------------------------------------------------
# Our sources
# ------------------------------------------------------------------
OUR_SRCS := \
    src/main.c \
    src/ps_libc.c

# ------------------------------------------------------------------
# Doomgeneric sources — shell glob (proven to work on this CI).
# Excludes: doomgeneric_* (platform backends), i_allegro/sdl/soso/xlib/oal,
#           i_main (has its own main), i_psp, i_videohr.
# Keeps: doomgeneric.c, d_main.c, i_sound.c, i_video.c, i_input.c,
#        i_timer.c, and everything else.
# ------------------------------------------------------------------
DOOM_SRCS := $(shell \
    ls $(DOOM_DIR)/*.c 2>/dev/null | \
    grep -v -E '(doomgeneric_|i_allegro|i_sdl|i_soso|i_xlib|i_oal|i_main|i_psp|i_videohr)' \
)

SRCS := $(OUR_SRCS) $(DOOM_SRCS)
OBJS := $(SRCS:.c=.o)

# ------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------
.PHONY: all clean hex size check-doom debug-src relocs sections

all: doom_ps.bin doom_ps.elf

# ------------------------------------------------------------------
# Early sanity check — abort with a clear message if we can't find
# doomgeneric sources.
# ------------------------------------------------------------------
check-doom:
	@if [ ! -d "$(DOOM_DIR)" ]; then \
	    echo ""; \
	    echo "[!] $(DOOM_DIR)/ not found."; \
	    echo "    Run:  bash build.sh"; \
	    echo "    or:   git clone --depth=1 https://github.com/ozkl/doomgeneric $(DOOM_DIR)"; \
	    echo ""; \
	    exit 1; \
	fi
	@if [ -z "$(DOOM_SRCS)" ]; then \
	    echo ""; \
	    echo "[!] No doomgeneric .c files found in $(DOOM_DIR)/"; \
	    echo "    Directory contents:"; \
	    ls -la $(DOOM_DIR)/ 2>&1 | head -30; \
	    echo ""; \
	    exit 1; \
	fi
	@echo "[OK] Found $(words $(DOOM_SRCS)) doomgeneric sources"
	@echo "[OK] Found $(words $(OUR_SRCS)) our sources"

# ------------------------------------------------------------------
# Compile
# ------------------------------------------------------------------
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------
# Link
# ------------------------------------------------------------------
doom_ps.elf: check-doom $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@
	@echo ""
	@echo "===== ELF header ====="
	@readelf -h doom_ps.elf | grep -E 'Type|Machine|Entry'
	@echo ""
	@echo "===== Program headers ====="
	@readelf -l doom_ps.elf | grep -E 'LOAD|Flags'

# ------------------------------------------------------------------
# Binary — dump the whole segment (not just .text) so .rela_out
# (relocation table) is included.
# ------------------------------------------------------------------
doom_ps.bin: doom_ps.elf
	$(OBJCOPY) -O binary doom_ps.elf doom_ps.bin
	@echo ""
	@ls -l doom_ps.bin
	@echo ""
	@SIZE=$$(stat -c %s doom_ps.bin); \
	if [ "$$SIZE" -lt 620000 ]; then \
	    echo "[!] WARN: doom_ps.bin is $$SIZE bytes (< 620 KB)."; \
	    echo "    Console launcher expects >= ~623 KB."; \
	fi

# ------------------------------------------------------------------
# Hex dump for pasting into a Lua payload
# ------------------------------------------------------------------
hex: doom_ps.bin
	@xxd -p doom_ps.bin | tr -d '\n' | sed 's/../& /g' > doom_ps.hex
	@echo "Wrote doom_ps.hex ($$(stat -c %s doom_ps.hex) bytes)"

# ------------------------------------------------------------------
# Info / debug
# ------------------------------------------------------------------
size: doom_ps.bin
	@echo "--- doom_ps.bin ---"
	@stat -c '%s bytes' doom_ps.bin
	@echo ""
	@size doom_ps.elf

relocs: doom_ps.elf
	@echo "--- .rela.dyn / .rela_out entries (first 40) ---"
	@readelf -r doom_ps.elf 2>/dev/null | head -40 || echo "(none)"
	@echo ""
	@echo "--- total R_X86_64 relocations ---"
	@readelf -r doom_ps.elf 2>/dev/null | grep -c R_X86_64 || true

sections: doom_ps.elf
	@readelf -S doom_ps.elf

debug-src:
	@echo "DOOM_DIR  = $(DOOM_DIR)"
	@echo ""
	@echo "OUR_SRCS  ="
	@for f in $(OUR_SRCS); do echo "  $$f"; done
	@echo ""
	@echo "DOOM_SRCS ="
	@for f in $(DOOM_SRCS); do echo "  $$f"; done
	@echo ""
	@echo "SRCS count = $(words $(SRCS))"
	@echo "OBJS count = $(words $(OBJS))"

clean:
	rm -f $(OBJS) doom_ps.elf doom_ps.bin doom_ps.hex
	@find $(DOOM_DIR) -name '*.o' -delete 2>/dev/null || true
	@echo "clean done"
