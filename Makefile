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
DG_ALL_C := $(wildcard $(DOOM_DIR)/*.c)

# Platform glue: doomgeneric_allegro.c, doomgeneric_emscripten.c,
# doomgeneric_sdl.c, doomgeneric_sdl2.c, doomgeneric_soso.c,
# doomgeneric_sosox.c, doomgeneric_win.c, doomgeneric_xlib.c ...
# (note: does NOT match doomgeneric.c — the core — because of the dot)
DG_PLATFORM_C := $(wildcard $(DOOM_DIR)/doomgeneric_*.c)

# Host audio backends (require allegro/sdl/soso/xlib/openal headers)
DG_HOSTAUD_C := \
    $(wildcard $(DOOM_DIR)/i_allegro*.c) \
    $(wildcard $(DOOM_DIR)/i_sdl*.c) \
    $(wildcard $(DOOM_DIR)/i_soso*.c) \
    $(wildcard $(DOOM_DIR)/i_xlib*.c) \
    $(wildcard $(DOOM_DIR)/i_oal*.c)

# Other host-specific files
DG_OTHER_C := \
    $(wildcard $(DOOM_DIR)/i_psp*.c) \
    $(wildcard $(DOOM_DIR)/i_videohr*.c) \
    $(DOOM_DIR)/i_main.c

DG_C := $(filter-out $(DG_PLATFORM_C) $(DG_HOSTAUD_C) $(DG_OTHER_C),$(DG_ALL_C))
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
