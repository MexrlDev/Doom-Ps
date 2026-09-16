#!/usr/bin/env python3
"""
wad_sender.py  —  stream a WAD file to the doom-ps TCP receiver.

Usage:
    python3 wad_sender.py <console_ip> <wad_path> [--port PORT]

Protocol (matches recv_wad() in src/main.c):
    connect  →  8-byte little-endian size  →  raw WAD bytes  →  close
"""
import argparse, os, socket, struct, sys, time

DEFAULT_PORT = 5000
CHUNK        = 64 * 1024

def send(host, port, path):
    if not os.path.isfile(path):
        sys.exit(f"[!] Not found: {path}")
    size = os.path.getsize(path)
    print(f"[*] {path}  ({size:,} bytes)")
    print(f"[*] Connecting to {host}:{port} …", flush=True)

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(30)
    deadline = time.time() + 30
    while True:
        try:
            s.connect((host, port)); break
        except (ConnectionRefusedError, OSError):
            if time.time() >= deadline:
                sys.exit("[!] Timeout — is doom_launcher.lua running?")
            time.sleep(1); print("  retrying…", flush=True)

    s.settimeout(None)
    print("[*] Connected — sending header…", flush=True)
    s.sendall(struct.pack("<Q", size))

    sent = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk: break
            s.sendall(chunk)
            sent += len(chunk)
            print(f"\r  {sent:,}/{size:,} ({sent*100//size}%)", end="", flush=True)
    print(f"\n[+] Done — {sent:,} bytes sent.")
    s.close()

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("host")
    p.add_argument("wad_path")
    p.add_argument("--port", "-p", type=int, default=DEFAULT_PORT)
    a = p.parse_args()
    send(a.host, a.port, a.wad_path)
