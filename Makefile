# doom-ps/Makefile

DOOM_DIR  := doomgeneric/doomgeneric
BUILD_DIR := build

CC      ?= gcc
OBJCOPY ?= objcopy

CFLAGS := -Os -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer \
          -fcf-protection=none -fno-exceptions \
          -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Wall -Wno-unused-function -Wno-unused-variable \
          -Isrc -I$(DOOM_DIR)

LDFLAGS := -T linker.ld -nostdlib -nostartfiles \
           -Wl,--build-id=none -Wl,--no-dynamic-linker \
           -Wl,-z,norelro -no-pie

# ---- our sources ----
OUR_C := src/main.c src/i_sound_ps.c
OUR_O := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(OUR_C))

# ---- doomgeneric sources ----
# Grab every .c, then drop all platform backends.
# The pattern `doomgeneric_*.c` matches doomgeneric_allegro.c,
# doomgeneric_emscripten.c, doomgeneric_sdl.c, doomgeneric_sdl2.c,
# doomgeneric_soso.c, doomgeneric_sosox.c, doomgeneric_win.c,
# doomgeneric_xlib.c, etc.  It does NOT match doomgeneric.c itself
# (dot, not underscore), so the core stays in.
DG_ALL_C := $(wildcard $(DOOM_DIR)/*.c)
DG_BAD_C := $(wildcard $(DOOM_DIR)/doomgeneric_*.c)
DG_C     := $(filter-out $(DG_BAD_C),$(DG_ALL_C))

# If your i_sound_ps.c fully replaces doomgeneric's own i_sound.c,
# uncomment the next line to avoid duplicate I_Sound*/I_Music* symbols:
# DG_C := $(filter-out $(DOOM_DIR)/i_sound.c,$(DG_C))

DG_O := $(patsubst $(DOOM_DIR)/%.c,$(BUILD_DIR)/dg/%.o,$(DG_C))

OBJS := $(OUR_O) $(DG_O)

# ---- targets ----
.PHONY: all clean hex

all: doom_ps.elf doom_ps.bin

doom_ps.elf: $(OBJS) linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

doom_ps.bin: doom_ps.elf
	$(OBJCOPY) -O binary $< $@

hex: doom_ps.bin
	od -An -v -tx1 doom_ps.bin | tr -d ' \n' > doom_ps.hex
	@echo
	@echo "[+] doom_ps.hex: $$(wc -c < doom_ps.hex) chars"
	@echo "[+] doom_ps.bin: $$(wc -c < doom_ps.bin) bytes"

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dg/%.o: $(DOOM_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) doom_ps.elf doom_ps.bin doom_ps.hex
