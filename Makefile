# ============================================================
# doom-ps/Makefile
#
# Builds doomgeneric (cloned by build.sh) + our shellcode into a
# freestanding x86-64 blob for Luac0re.
#
# KEY CHANGE (v19):
#   LDFLAGS now uses  -pie  instead of  -no-pie.
#   This makes the linker emit .rela.dyn relocation records into the
#   output.  Our _start walks those records and applies load_base to
#   every R_X86_64_RELATIVE entry, fixing absolute pointers stored in
#   .data (e.g. defaults[].location = &screenblocks).  Without this,
#   Doom crashes silently inside M_LoadDefaults.
# ============================================================

DOOM_DIR := doomgeneric

CC      := gcc
OBJCOPY := objcopy
PYTHON  := python3

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
# Linker flags — CRITICAL: -pie, NOT -no-pie
# ------------------------------------------------------------------
LDFLAGS := \
    -T linker.ld \
    -nostdlib -nostartfiles \
    -Wl,--build-id=none \
    -Wl,--no-dynamic-linker \
    -Wl,-z,norelro \
    -pie

# ------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------
OUR_SRCS := \
    src/main.c \
    src/ps_libc.c

# doomgeneric source files — everything except platform backends
DOOM_SRCS := $(shell \
    ls $(DOOM_DIR)/*.c 2>/dev/null | \
    grep -v -E '(doomgeneric_|i_allegro|i_sdl|i_soso|i_xlib|i_oal|i_main|i_psp|i_videohr)' \
)

SRCS := $(OUR_SRCS) $(DOOM_SRCS)

# ------------------------------------------------------------------
# Objects
# ------------------------------------------------------------------
OBJS := $(SRCS:.c=.o)

# ------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------
.PHONY: all clean hex size check-doom

all: doom_ps.bin doom_ps.elf

check-doom:
	@if [ ! -d "$(DOOM_DIR)" ]; then \
	    echo "[!] $(DOOM_DIR)/ not found."; \
	    echo "    Run:  bash build.sh"; \
	    echo "    Or:   git clone --depth=1 https://github.com/ozkl/doomgeneric $(DOOM_DIR)"; \
	    exit 1; \
	fi

%.o: %.c check-doom
	$(CC) $(CFLAGS) -c $< -o $@

doom_ps.elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@
	@echo ""
	@echo "===== ELF header ====="
	readelf -h doom_ps.elf | grep -E 'Type|Machine|Entry'
	@echo ""
	@echo "===== Program headers ====="
	readelf -l doom_ps.elf | grep -E 'LOAD|Flags'

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

# Generate hex string for pasting into doom_launcher.lua
hex: doom_ps.bin
	@xxd -p doom_ps.bin | tr -d '\n' | sed 's/../& /g' > doom_ps.hex
	@echo "Wrote doom_ps.hex ($$(stat -c %s doom_ps.hex) bytes)"

size: doom_ps.bin
	@echo "--- doom_ps.bin ---"
	@stat -c '%s bytes' doom_ps.bin
	@size doom_ps.elf

clean:
	rm -f $(OBJS) doom_ps.elf doom_ps.bin doom_ps.hex
	@find $(DOOM_DIR) -name '*.o' -delete 2>/dev/null || true
	@echo "clean done"

# ------------------------------------------------------------------
# Debug helpers
# ------------------------------------------------------------------
relocs: doom_ps.elf
	@echo "--- .rela.dyn entries ---"
	@readelf -r doom_ps.elf 2>/dev/null | head -40 || echo "(none)"

sections: doom_ps.elf
	@echo "--- Section headers ---"
	@readelf -S doom_ps.elf

.PHONY: relocs sections
