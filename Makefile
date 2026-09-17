# ============================================================
# doom-ps/Makefile
#
# Builds doomgeneric + our shellcode into a freestanding x86-64
# blob for Luac0re.
#
# Uses GNU make's wildcard + filter-out (not $(shell ls | grep))
# so the doomgeneric source list is discovered reliably on all
# platforms, including GitHub Actions runners.
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
# Our source files
# ------------------------------------------------------------------
OUR_SRCS := \
    src/main.c \
    src/ps_libc.c

# ------------------------------------------------------------------
# doomgeneric sources — everything except platform backends.
#
# The `%` in filter-out is a make wildcard, not a shell glob.
# doomgeneric/doomgeneric.c   → KEPT   (has Create/Tick/ScreenBuffer)
# doomgeneric/doomgeneric_*.c → DROPPED (platform backends)
# doomgeneric/i_main.c        → DROPPED (its own main())
# doomgeneric/i_sdl.c etc.    → DROPPED
# doomgeneric/i_sound.c etc.  → KEPT
# ------------------------------------------------------------------
ALL_DOOM_C := $(wildcard $(DOOM_DIR)/*.c)

DOOM_SRCS := $(filter-out \
    $(DOOM_DIR)/doomgeneric_%.c \
    $(DOOM_DIR)/i_allegro%.c \
    $(DOOM_DIR)/i_sdl%.c \
    $(DOOM_DIR)/i_soso%.c \
    $(DOOM_DIR)/i_xlib%.c \
    $(DOOM_DIR)/i_oal%.c \
    $(DOOM_DIR)/i_main.c \
    $(DOOM_DIR)/i_psp%.c \
    $(DOOM_DIR)/i_videohr%.c, \
    $(ALL_DOOM_C))

SRCS := $(OUR_SRCS) $(DOOM_SRCS)
OBJS := $(SRCS:.c=.o)

# ------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------
.PHONY: all clean hex size check-doom list-src relocs sections

all: doom_ps.bin doom_ps.elf

check-doom:
	@if [ ! -d "$(DOOM_DIR)" ]; then \
	    echo "[!] $(DOOM_DIR)/ not found."; \
	    echo "    Run:  bash build.sh"; \
	    exit 1; \
	fi

list-src: check-doom
	@echo "Our sources:"
	@for f in $(OUR_SRCS); do echo "  $$f"; done
	@echo "Doomgeneric sources:"
	@for f in $(DOOM_SRCS); do echo "  $$f"; done
	@echo ""
	@echo "Total C files: $(words $(SRCS))"

%.o: %.c check-doom
	$(CC) $(CFLAGS) -c $< -o $@

doom_ps.elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@
	@echo ""
	@echo "===== ELF header ====="
	@readelf -h doom_ps.elf | grep -E 'Type|Machine|Entry'
	@echo ""
	@echo "===== Program headers ====="
	@readelf -l doom_ps.elf | grep -E 'LOAD|Flags'

doom_ps.bin: doom_ps.elf
	$(OBJCOPY) -O binary -j .text doom_ps.bin
	@echo ""
	@ls -l doom_ps.bin
	@echo ""
	@SIZE=$$(stat -c %s doom_ps.bin); \
	if [ "$$SIZE" -lt 620000 ]; then \
	    echo "[!] WARN: doom_ps.bin is $$SIZE bytes."; \
	    echo "    Console launcher expects >= ~623 KB."; \
	fi

hex: doom_ps.bin
	@xxd -p doom_ps.bin | tr -d '\n' | sed 's/../& /g' > doom_ps.hex
	@echo "Wrote doom_ps.hex ($$(stat -c %s doom_ps.hex) bytes)"

size: doom_ps.bin
	@echo "--- doom_ps.bin ---"
	@stat -c '%s bytes' doom_ps.bin
	@size doom_ps.elf

relocs: doom_ps.elf
	@echo "--- .rela.dyn entries (first 40) ---"
	@readelf -r doom_ps.elf 2>/dev/null | head -40 || echo "(none)"
	@echo ""
	@echo "--- total reloc count ---"
	@readelf -r doom_ps.elf 2>/dev/null | grep -c R_X86_64 || true

sections: doom_ps.elf
	@readelf -S doom_ps.elf

clean:
	rm -f $(OBJS) doom_ps.elf doom_ps.bin doom_ps.hex
	@find $(DOOM_DIR) -name '*.o' -delete 2>/dev/null || true
	@echo "clean done"
