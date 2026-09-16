CC      = gcc
OBJCOPY = objcopy

# Exact flags from EmuC0re/Exp-C0re Makefile — proven to work on PS4/PS5.
# -fpie (not -fPIC) matches what EmuC0re uses.
# -nostdlib matches the real linker invocation.
CFLAGS  = -Os -ffreestanding -fno-stack-protector -fno-builtin \
          -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
          -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Wall -Wno-unused-function -Wno-unused-variable \
          -Isrc -I$(DG_DIR)/doomgeneric

LDFLAGS = -T linker.ld -nostdlib -nostartfiles -static \
          -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,norelro -no-pie

# ---- doomgeneric ----
DG_DIR  = doomgeneric
DG_SRCDIR = $(DG_DIR)/doomgeneric

# All .c files from doomgeneric except the platform-specific ones it ships
# (SDL, xlib, windows, cocoa, dummy) and the audio/video backends we replace.
DG_ALL   = $(wildcard $(DG_SRCDIR)/*.c)
DG_EXCL  = \
    $(DG_SRCDIR)/doomgeneric_sdl.c   \
    $(DG_SRCDIR)/doomgeneric_xlib.c  \
    $(DG_SRCDIR)/doomgeneric_win.c   \
    $(DG_SRCDIR)/doomgeneric_cocoa.c \
    $(DG_SRCDIR)/doomgeneric_dummy.c \
    $(DG_SRCDIR)/i_video.c           \
    $(DG_SRCDIR)/i_sound.c           \
    $(DG_SRCDIR)/i_music.c
DG_SRCS  = $(filter-out $(DG_EXCL), $(DG_ALL))

# Our platform layer
PS_SRCS  = src/main.c src/i_sound_ps.c

SRCS     = $(PS_SRCS) $(DG_SRCS)
OBJS     = $(SRCS:.c=.o)
TARGET   = doom_ps

all: $(TARGET).bin

# Clone doomgeneric if not already present
$(DG_DIR):
	git clone --depth=1 https://github.com/ozkl/doomgeneric $(DG_DIR)

# Compile our platform files
src/%.o: src/%.c src/core.h src/doomgeneric_ps.h | $(DG_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile doomgeneric files
$(DG_SRCDIR)/%.o: $(DG_SRCDIR)/%.c | $(DG_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Linked: $@"

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

# Emit Lua-embeddable hex string (paste into doom_launcher.lua)
hex: $(TARGET).bin
	@xxd -i $(TARGET).bin \
	  | grep -v "^unsigned\|^#define" \
	  | tr -d ' \n' \
	  | tr -d ','
	@echo

clean:
	rm -f src/*.o $(DG_SRCDIR)/*.o $(TARGET).elf $(TARGET).bin

.PHONY: all hex clean
