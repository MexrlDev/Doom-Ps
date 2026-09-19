# doom-ps
* Beta v40 - Fully single player 
* Can you give me [one more day](https://open.spotify.com/track/2n0XjGSB8TO42W8k1G1F3a?si=lJz4SXNZTRCM1W_LsohkyQ&utm_source=copy-link)..

Doom (via [doomgeneric](https://github.com/ozkl/doomgeneric)) running as
homebrew on PS4 / PS5 through the **Luac0re** shellcode loader.

Doom-Ps is based on [EmuC0re](https://github.com/egycnq/EmuC0re) by [egycnq](https://github.com/egycnq),
[Exp-C0re](https://github.com/MexrlDev/Exp-C0re) by MexrlDev,
[LuaC0re](https://github.com/Gezine/Luac0re) by [Gezine](https://github.com/Gezine),
and [doomgeneric](https://github.com/ozkl/doomgeneric) by [ozkl](https://github.com/ozkl).

---

# Project Photos

<p align="center">
  <img src="https://raw.githubusercontent.com/MexrlDev/Doom-Ps/refs/heads/main/.photos/IMG_9446.jpeg" width="500">
</p>

<details>
<summary>Show more pics</summary>

<br>

<p align="center">
  <img src="https://raw.githubusercontent.com/MexrlDev/Doom-Ps/refs/heads/main/.photos/IMG_9444.jpeg" width="500">
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/MexrlDev/Doom-Ps/refs/heads/main/.photos/IMG_9445.jpeg" width="500">
</p>

</details>

---

## Files

```
doom-ps/
├── src/
│   ├── core.h              types, offsets, native_call (from EmuC0re)
│   ├── main.c              _start + all doomgeneric callbacks
│   ├── doomgeneric_ps.h    public API header
│   ├── i_sound_ps.c        audio backend (dedicated thread)
│   └── ps_libc.c           freestanding libc (printf, malloc, fopen, etc.)
├── Makefile
├── linker.ld               base-0 single RWX segment (from EmuC0re)
├── doom_launcher.lua       Luac0re payload
├── doom-luncher.py         launcher: streams shellcode + WAD over TCP
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

Downloads: `doom_ps.elf`, `doom_ps.bin`, `doom_ps.hex` ~~(hex string
ready to paste into doom_launcher.lua).~~

---

## Deploy

### 1. Edit `doom_launcher.lua`

```lua
local PC_IP    = "192.168.1.100"  -- your PC
local LOG_PORT = 9027
local WAD_PORT = 5000
local shellcode_hex = "..."       -- paste from `make hex` or doom_ps.hex
```

### 2. Run the launcher from your PC

```sh
python3 doom_launcher.py 192.168.1.50 /path/to/DOOM.WAD
```
### 2. Run the launcher from your phone
1. make a folder and add the python in it and also put the bin in the same folder as the python, and put the wad in /wad folder. don’t forget to change the Python to ur ip, and put the wad file name you want to send
2. Press the start button on either iPhone Pythonica, Android PyCode.

3. [Instructions on Pythonica & PyCode](https://github.com/MexrlDev/repo/tree/main/Mobile-Python-Tools#how-to-setup-the-scripts-in-this-folder)

The launcher:

1. Sends `doom_launcher.lua` to the console on port 9026
2. TCP-scans ports 5001..5020 for the shellcode receiver
3. Streams `doom_ps.bin` to that port
4. Streams the WAD to port 5000

It retries for 25 s so you don't need to time it perfectly.
Shellcode writes the WAD to `/av_contents/content_tmp/doom.wad`
(falls back to `/savedata0/doom.wad`), then boots Doom.

---

## Controls

| Button       | Action                    |
|--------------|---------------------------|
| D-Pad        | Move / turn               |
| Cross ×      | Fire / Back (Doom menu)   |
| Square □     | Use / open door           |
| Triangle △   | Run (hold)                |
| Circle ○     | Enter / confirm           |
| Options      | Escape / main menu        |
| R1           | Save menu (F2)            |
| L1           | Load menu (F3)            |
| R2           | Next weapon               |
| L2           | Previous weapon           |
| TouchPad     | Open Menu           |

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
# Doom-PS: Supported WAD List

| WAD File | Game Title | Type | Notes |
|---|---|---|---|
| `DOOM1.WAD` | Doom Shareware 1.9 | Shareware | Free to distribute. Contains Episode 1 only. |
| `DOOM.WAD` | The Ultimate Doom | IWAD | Includes the original three episodes plus the fourth episode (Thy Flesh Consumed). |
| `doom.wad` | Doom (Registered/Retail) | Retail | The original 3-episode release. |
| `doom2.wad` | Doom II: Hell on Earth | Commercial | 32 levels, new weapons & enemies. |
| `plutonia.wad` | Final Doom: Plutonia Experiment | Commercial | Known for high difficulty. |
| `tnt.wad` | Final Doom: TNT: Evilution | Commercial | Another official Final Doom episode. |
| `freedoom1.wad` | Freedoom: Phase 1 | Free | Free alternative to Doom 1. |
| `freedoom2.wad` | Freedoom: Phase 2 | Free | Free alternative to Doom II. |
| `freedm.wad` | FreeDM | Free | A free deathmatch-focused IWAD. |
| `chex.wad` | Chex Quest | Shareware | A Doom-based game using the same engine. |
| `hacx.wad` | Hacx | Commercial | A total conversion using Doom II engine. |
| `heretic.wad` | Heretic | Retail | Fantasy-themed Doom engine game. |
| `heretic1.wad` | Heretic Shareware | Shareware | Shareware version of Heretic. |
| `hexen.wad` | Hexen | Commercial | Uses a modified Doom engine. |
| `strife1.wad` | Strife | Commercial | RPG-flavored Doom engine game. |

---
## Issues
1. SFX + Music cutting but stable (if music is lowest muted in menu option > sound volume and kept SFX only it’ll work stable .
2. switching from WAD to another via Quit Game in doom. switching to another WAD makes music doesn’t work for the new game BUTTT. SFX works so eh. fix it later max..

---
## Credits

* **MexrlDev** — Project Development

**Special Thanks To**
- [egycnq](https://github.com/egycnq) for [EmuC0re](https://github.com/egycnq/EmuC0re)
- [Gezine](https://github.com/Gezine) for [Luac0re](https://github.com/Gezine/Luac0re)
- [ozkl](https://github.com/ozkl) for [doomgeneric](https://github.com/ozkl/doomgeneric)
- [shahrilnet](https://github.com/shahrilnet) & [n0llptr](https://github.com/n0llptr) for [remote_lua_loader](https://github.com/shahrilnet/remote_lua_loader)
- Claude & Deepseek models for code development and bug research

---

#
