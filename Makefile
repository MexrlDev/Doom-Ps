# ============================================================
# doom-ps/Makefile
#
# v24: -O2 (was -Os) for faster Doom rendering.
#      Excludes doomgeneric/i_sound.c so our i_sound_ps.c is used.
# ============================================================

DOOM_ROOT := doomgeneric

# Auto-detect the directory containing doomgeneric.c
DOOM_DIR := $(shell \
    for d in "$(DOOM_ROOT)" "$(DOOM_ROOT)/$(DOOM_ROOT)" "." ; do \
        if [ -f "$$d/doomgeneric.c" ]; then echo "$$d"; exit 0; fi; \
    done; \
    echo "$(DOOM_ROOT)" \
)

CC      := gcc
OBJCOPY := objcopy

# ------------------------------------------------------------------
# C flags — -O2 for speed (was -Os)
# ------------------------------------------------------------------
CFLAGS := \
    -O2 \
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

LDFLAGS := \
    -T linker.ld \
    -nostdlib -nostartfiles \
    -Wl,--build-id=none \
    -Wl,--no-dynamic-linker \
    -Wl,-z,norelro \
    -pie

# ------------------------------------------------------------------
# Our sources — i_sound_ps.c replaces doomgeneric's i_sound.c
# ------------------------------------------------------------------
OUR_SRCS := \
    src/main.c \
    src/ps_libc.c \
    src/i_sound_ps.c

# ------------------------------------------------------------------
# Doomgeneric sources — EXCLUDES i_sound.c
# ------------------------------------------------------------------
DOOM_SRCS := $(shell \
    ls $(DOOM_DIR)/*.c 2>/dev/null | \
    grep -v -E '(doomgeneric_|i_allegro|i_sdl|i_soso|i_xlib|i_oal|i_main|i_psp|i_videohr|i_sound\.c$$)' \
)

SRCS := $(OUR_SRCS) $(DOOM_SRCS)
OBJS := $(SRCS:.c=.o)

# ------------------------------------------------------------------
.PHONY: all clean hex size check-doom debug-src relocs sections

all: doom_ps.bin doom_ps.elf

check-doom:
	@if [ ! -f "$(DOOM_DIR)/doomgeneric.c" ]; then \
	    echo "[!] doomgeneric.c not found in $(DOOM_DIR)/"; \
	    echo "    Run: bash build.sh"; \
	    exit 1; \
	fi
	@echo "[OK] DOOM_DIR       = $(DOOM_DIR)"
	@echo "[OK] doomgeneric.c  = $(DOOM_DIR)/doomgeneric.c"
	@echo "[OK] Doom C sources = $(words $(DOOM_SRCS))"
	@echo "[OK] Our C sources  = $(words $(OUR_SRCS))"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

doom_ps.elf: check-doom $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@
	@echo ""
	@readelf -h doom_ps.elf | grep -E 'Type|Machine|Entry'

doom_ps.bin: doom_ps.elf
	$(OBJCOPY) -O binary doom_ps.elf doom_ps.bin
	@ls -l doom_ps.bin

hex: doom_ps.bin
	@xxd -p doom_ps.bin | tr -d '\n' | sed 's/../& /g' > doom_ps.hex
	@echo "Wrote doom_ps.hex"

size: doom_ps.bin
	@stat -c '%s bytes' doom_ps.bin
	@size doom_ps.elf

relocs: doom_ps.elf
	@readelf -r doom_ps.elf 2>/dev/null | grep -c R_X86_64 || true

debug-src:
	@echo "OUR_SRCS  ="; for f in $(OUR_SRCS); do echo "  $$f"; done
	@echo "DOOM_SRCS = $(words $(DOOM_SRCS)) files"
	@echo "SRCS count = $(words $(SRCS))"

clean:
	rm -f $(OBJS) doom_ps.elf doom_ps.bin doom_ps.hex
	@find $(DOOM_DIR) -name '*.o' -delete 2>/dev/null || true
	@echo "clean done"
