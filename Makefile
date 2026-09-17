# ============================================================
# doom-ps/Makefile
#
# v19: auto-detects the directory containing doomgeneric.c
# (repo layout is `doomgeneric/doomgeneric/*.c` on current clones,
#  but auto-detect handles either layout).
# -pie for relocations; objcopy dumps whole segment (not just .text).
# ============================================================

DOOM_ROOT := doomgeneric

# ------------------------------------------------------------------
# Auto-detect the directory that actually contains doomgeneric.c.
# Checks (in order):
#   1. doomgeneric/doomgeneric.c             (flat layout)
#   2. doomgeneric/doomgeneric/doomgeneric.c (nested layout — usual)
#   3. .                                     (sources in repo root)
#   4. fallback to "doomgeneric" for a clear error message
# ------------------------------------------------------------------
DOOM_DIR := $(shell \
    for d in "$(DOOM_ROOT)" "$(DOOM_ROOT)/$(DOOM_ROOT)" "." ; do \
        if [ -f "$$d/doomgeneric.c" ]; then echo "$$d"; exit 0; fi; \
    done; \
    echo "$(DOOM_ROOT)" \
)

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
# Doomgeneric sources
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
# Sanity check
# ------------------------------------------------------------------
check-doom:
	@if [ ! -d "$(DOOM_ROOT)" ]; then \
	    echo ""; \
	    echo "[!] $(DOOM_ROOT)/ not found."; \
	    echo "    Run:  bash build.sh"; \
	    echo "    or:   git clone --depth=1 https://github.com/ozkl/doomgeneric $(DOOM_ROOT)"; \
	    echo ""; \
	    exit 1; \
	fi
	@if [ ! -f "$(DOOM_DIR)/doomgeneric.c" ]; then \
	    echo ""; \
	    echo "[!] doomgeneric.c not found.  DOOM_DIR=$(DOOM_DIR)"; \
	    echo ""; \
	    echo "    Looked in:"; \
	    echo "      $(DOOM_ROOT)/doomgeneric.c"; \
	    echo "      $(DOOM_ROOT)/$(DOOM_ROOT)/doomgeneric.c"; \
	    echo "      ./doomgeneric.c"; \
	    echo ""; \
	    echo "    Top-level directory listing:"; \
	    ls -la "$(DOOM_ROOT)/" 2>&1 | head -30; \
	    echo ""; \
	    exit 1; \
	fi
	@if [ -z "$(DOOM_SRCS)" ]; then \
	    echo ""; \
	    echo "[!] No .c files found in $(DOOM_DIR)/"; \
	    ls -la "$(DOOM_DIR)/" 2>&1 | head -30; \
	    echo ""; \
	    exit 1; \
	fi
	@echo "[OK] DOOM_DIR       = $(DOOM_DIR)"
	@echo "[OK] doomgeneric.c  = $(DOOM_DIR)/doomgeneric.c"
	@echo "[OK] Doom C sources = $(words $(DOOM_SRCS))"
	@echo "[OK] Our C sources  = $(words $(OUR_SRCS))"

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
# Binary — dump the entire segment so .rela_out is included.
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
	@echo "--- relocations (first 40) ---"
	@readelf -r doom_ps.elf 2>/dev/null | head -40 || echo "(none)"
	@echo ""
	@echo "--- total R_X86_64 relocations ---"
	@readelf -r doom_ps.elf 2>/dev/null | grep -c R_X86_64 || true

sections: doom_ps.elf
	@readelf -S doom_ps.elf

debug-src:
	@echo "DOOM_ROOT = $(DOOM_ROOT)"
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
