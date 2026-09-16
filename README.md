# doom-ps

Doom (via [doomgeneric](https://github.com/ozkl/doomgeneric)) running as
homebrew on PS4 / PS5 through the **Luac0re** shellcode loader.

Doom-Ps is based of [EmuC0re](https://github.com/egycnq/EmuC0re) by [egycnq](https://github.com/egycnq) and also based on [Exp-C0re](https://github.com/MexrlDev/Exp-C0re) 
and based of [LuaC0re](https://github.com/Gezine/Luac0re) by [Gezine](https://github.com/Gezine) and also based on [doomgeneric](https://github.com/ozkl/doomgeneric)) 

---

## Files

```
doom-ps/
├── src/
│   ├── core.h              types, offsets, native_call (from EmuC0re)
│   ├── main.c              _start + all doomgeneric callbacks
│   ├── doomgeneric_ps.h    public API header
│   └── i_sound_ps.c        audio backend
├── Makefile
├── linker.ld               base-0 single RWX segment (from EmuC0re)
├── doom_launcher.lua       Luac0re payload (mirrors nes.lua)
├── wad_sender.py           streams WAD to console over TCP
├── log_server.py           receives UDP debug log from shellcode
├── build.sh                one-shot clone + build
└── .github/workflows/
    └── build.yml           GitHub Actions CI
```

---

## Build (local, Linux x86-64)

```sh
bash build.sh
# or manually:
git clone --depth=1 https://github.com/ozkl/doomgeneric doomgeneric
make -j$(nproc)
```

Produces `doom_ps.elf` + `doom_ps.bin`.

---

## Build (GitHub Actions)

Push to GitHub → Actions tab → **Build doom-ps** → Run workflow.

Downloads: `doom_ps.elf`, `doom_ps.bin`, `doom_ps.hex` (hex string
ready to paste into doom_launcher.lua).

---

## Deploy

### 1. Edit doom_launcher.lua

```lua
local PC_IP    = "192.168.1.100"  -- your PC
local LOG_PORT = 9027
local WAD_PORT = 5000
local shellcode_hex = "..."       -- paste from `make hex` or doom_ps.hex
```

### 2. Start log receiver on PC

```sh
python3 log_server.py --port 9027
```

### 3. Send Lua payload to console via Luac0re

### 4. Send the WAD

```sh
python3 wad_sender.py 192.168.1.50 /path/to/DOOM.WAD
```

wad_sender retries for 30 s so you don't need to time it perfectly.
Shellcode writes to `/savedata0/doom.wad` then launches Doom.

---

## Controls

| Button       | Action               |
|--------------|----------------------|
| D-Pad        | Move / turn          |
| Cross ×      | Fire                 |
| Square □     | Use / open           |
| Circle ○     | Enter / confirm      |
| Options      | Escape / menu        |
| Triangle △   | Automap              |
| L1           | Run                  |
| R1           | Strafe               |
| Share + ×□○△ | Weapons 1–4          |
| L3 + ×□○     | Weapons 5–7          |

---

## EBOOT offsets

Set for **Star Wars Racer Revenge** (PS2-Classic host). If you use a
different game, update these three constants in `src/core.h`:

```c
#define GADGET_OFFSET    0x31AA9
#define EBOOT_GS_THREAD  0x057F89B0
#define EBOOT_VIDOUT     0x02d695d0
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Black screen | Check EBOOT_VIDOUT offset for your host game |
| Crash on start | Check EBOOT_GS_THREAD — wrong thread cancel target |
| WAD transfer stalls | Firewall blocking port 5000, or Lua not yet at accept() |
| No audio | Normal — audio_h < 0 means libSceAudioOut unavailable, game still runs |
| Linker errors in CI | doomgeneric API changed — check i_sound.h for `mixsound` symbol name |

---

## Credits
* MexrlDev - Project Development

**Special Thanks To**
 - [egycnq](https://github.com/egycnq) for [EmuC0re](https://github.com/egycnq/EmuC0re)
 - [Gezine](https://github.com/Gezine) for [Luac0re](https://github.com/Gezine/Luac0re)
 - Claude & Deepseek models for codes development bug researching

---


# Epeical
