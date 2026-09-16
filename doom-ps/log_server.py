#!/usr/bin/env python3
"""log_server.py — receive UDP debug messages from the shellcode."""
import argparse, datetime, socket

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", "-p", type=int, default=9027)
    a = p.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind((a.host, a.port))
        print(f"[log] Listening on {a.host}:{a.port} …")
        while True:
            data, addr = s.recvfrom(65535)
            ts  = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            msg = data.decode("utf-8", errors="replace").rstrip("\n")
            print(f"[{ts}] {addr[0]}  {msg}")

if __name__ == "__main__":
    main()
