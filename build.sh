#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
echo "[*] Checking tools…"
for cmd in gcc git objcopy; do
    command -v "$cmd" >/dev/null || { echo "MISSING: $cmd"; exit 1; }
done
echo "[*] Cloning doomgeneric…"
[ -d doomgeneric ] || git clone --depth=1 https://github.com/ozkl/doomgeneric doomgeneric
echo "[*] Building…"
make -j"$(nproc)"
echo ""
echo "[+] Build complete:"
ls -lh doom_ps.elf doom_ps.bin
echo ""
echo "Next: run 'make hex', paste into doom_launcher.lua, send Lua to console,"
echo "then: python3 wad_sender.py <console_ip> /path/to/DOOM.WAD"
