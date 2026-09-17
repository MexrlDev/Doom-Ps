#!/usr/bin/env python3
"""
doom_launcher.py — Doom-ps launcher (iPhone / Android / desktop).

v13: fixed LogServer._stop shadowing threading.Thread._stop (which is an
     internal method — setting self._stop = Event() breaks the Thread's
     own stop mechanics).  Renamed to _stop_event.
"""

import argparse, datetime, os, socket, struct, sys, threading, time

DEFAULT_PS5_IP    = "192.168.1.6" # < IP here
DEFAULT_LAUNCHER  = "doom_launcher.lua" # < the lua launcher name with extension
DEFAULT_SHELLCODE = "doom_ps.bin" # < generated bin name. same folder as python.
DEFAULT_WAD       = "wad/doom1.wad" # < Folder name / Wad name with extension
PAYLOAD_PORT      = 9026 # < Lua Core Port
WAD_PORT          = 5000 # Wad file receiver port
LOG_PORT          = 9027 # < debug log port
SC_PORT_LO        = 5001
SC_PORT_HI        = 5021
CHUNK             = 64 * 1024 # < chunks send per second


def find_file(name, subdirs=("payloads", "lua", "wad", ".")):
    if os.path.isfile(name):
        return name
    for sub in subdirs:
        cand = os.path.join(sub, os.path.basename(name))
        if os.path.isfile(cand):
            return cand
    return None


def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class LogServer(threading.Thread):
    """
    UDP debug-log listener.

    NOTE: we must NOT name the stop Event '_stop' — threading.Thread has
    an internal method by that name.  Overwriting it breaks Thread
    cleanup with "'Event' object is not callable".
    """
    def __init__(self, port, host="0.0.0.0"):
        super().__init__(daemon=True)
        self.host, self.port = host, port
        self._stop_event = threading.Event()   # <- renamed
        self.sock = None
        self.scport = None

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        bound = False
        for attempt in range(3):
            try:
                s.bind((self.host, self.port))
                bound = True
                break
            except OSError as e:
                if attempt == 2:
                    print(f"[log] WARN: cannot bind UDP "
                          f"{self.host}:{self.port} ({e}). "
                          f"Logs will not be shown.")
                    s.close()
                    return
                time.sleep(0.5)
        if not bound:
            return

        s.settimeout(0.5)
        self.sock = s
        print(f"[log] UDP listening on {self.host}:{self.port}", flush=True)

        while not self._stop_event.is_set():
            try:
                data, addr = s.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break

            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            msg = data.decode("utf-8", errors="replace").rstrip("\n")
            print(f"[{ts}] {addr[0]}  {msg}", flush=True)

            if msg.startswith("SCPORT "):
                try:
                    self.scport = int(msg.split()[1])
                except (ValueError, IndexError):
                    pass

        s.close()

    def stop(self):
        self._stop_event.set()
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


def send_payload(host, filepath, port):
    path = find_file(filepath)
    if not path:
        print(f"[!] Launcher not found: {filepath}")
        return False
    with open(path, "rb") as f:
        data = f.read()
    print(f"[1] Sending {os.path.basename(path)} "
          f"({len(data):,} bytes) -> {host}:{port}")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    try:
        s.connect((host, port))
        s.sendall(data)
        return True
    except Exception as e:
        print(f"[!] Payload send error: {e}")
        return False
    finally:
        s.close()


def stream_shellcode(host, path,
                     port_lo=SC_PORT_LO, port_hi=SC_PORT_HI,
                     per_port_timeout=0.5, total_timeout=25):
    if not os.path.isfile(path):
        print(f"[!] Shellcode not found: {path}")
        return False

    with open(path, "rb") as f:
        data = f.read()
    size = len(data)
    print(f"[sc] Scanning {host}:{port_lo}..{port_hi - 1} "
          f"for shellcode port...")

    deadline = time.time() + total_timeout
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        for port in range(port_lo, port_hi):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(per_port_timeout)
            try:
                s.connect((host, port))
            except (ConnectionRefusedError, OSError):
                s.close()
                continue

            print(f"[sc] Connected to {host}:{port}, "
                  f"sending {size:,} bytes")
            s.settimeout(None)
            try:
                sent = 0
                t0 = time.time()
                while sent < size:
                    chunk = data[sent:sent + CHUNK]
                    s.sendall(chunk)
                    sent += len(chunk)
                    pct = sent * 100 // size
                    print(f"\r[sc] {sent:,}/{size:,} ({pct}%)",
                          end="", flush=True)
                print()
                dt = max(time.time() - t0, 1e-6)
                print(f"[sc] Done — {sent:,} bytes in {dt:.1f}s "
                      f"({sent / dt / 1024:.0f} KB/s)")
                s.close()
                return True
            except OSError as e:
                print(f"\n[!] Shellcode send failed: {e}")
                s.close()
                break
        else:
            if attempt % 5 == 0:
                print(f"[sc]   ...no listener yet "
                      f"({int(time.time() - (deadline - total_timeout))}s)")
            time.sleep(0.3)

    print(f"[!] Shellcode port not found after {total_timeout}s")
    return False


def stream_file(host, port, path, label, connect_timeout=30):
    if not os.path.isfile(path):
        print(f"[!] {label} not found: {path}")
        return False
    size = os.path.getsize(path)
    print(f"[{label}] Sending {path} ({size:,} bytes) -> {host}:{port}")

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    deadline = time.time() + connect_timeout
    while True:
        try:
            s.connect((host, port))
            break
        except (ConnectionRefusedError, OSError) as e:
            if time.time() >= deadline:
                s.close()
                print(f"[!] {label} connect timeout: {e}")
                return False
            time.sleep(1)
    s.settimeout(None)

    if label == "wad":
        s.sendall(struct.pack("<Q", size))

    sent, t0 = 0, time.time()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            s.sendall(chunk)
            sent += len(chunk)
            pct = sent * 100 // size
            print(f"\r[{label}] {sent:,}/{size:,} ({pct}%)",
                  end="", flush=True)
    print()
    s.close()
    dt = max(time.time() - t0, 1e-6)
    print(f"[{label}] Done — {sent:,} bytes in {dt:.1f}s "
          f"({sent / dt / 1024:.0f} KB/s)")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default=DEFAULT_PS5_IP)
    ap.add_argument("wad",  nargs="?", default=DEFAULT_WAD)
    ap.add_argument("--launcher",  "-l", default=DEFAULT_LAUNCHER)
    ap.add_argument("--shellcode", "-s", default=DEFAULT_SHELLCODE)
    ap.add_argument("--payload-port", type=int, default=PAYLOAD_PORT)
    ap.add_argument("--wad-port",     type=int, default=WAD_PORT)
    ap.add_argument("--log-port",     type=int, default=LOG_PORT)
    ap.add_argument("--local-ip",     default=None)
    ap.add_argument("--no-log",       action="store_true")
    ap.add_argument("--no-wad",       action="store_true")
    ap.add_argument("--no-shellcode", action="store_true")
    ap.add_argument("--scport",       type=int, default=None,
                    help="Skip TCP scan and use this shellcode port")
    ap.add_argument("--scport-wait",  type=int, default=25,
                    help="Total seconds to spend finding the SC port")
    ap.add_argument("--wad-delay",    type=float, default=2.0,
                    help="Seconds to wait after shellcode send before "
                         "starting the WAD transfer")
    a = ap.parse_args()

    print("=" * 60)
    print(" Doom-ps launcher — TCP port scan (no UDP control)")
    print("=" * 60)

    sc_path = None
    wad_path = None
    if not a.no_shellcode:
        sc_path = find_file(a.shellcode)
        if not sc_path:
            print(f"[!] Shellcode not found: {a.shellcode}")
            return 1
        print(f"[*] Shellcode: {sc_path} "
              f"({os.path.getsize(sc_path):,} bytes)")
    if not a.no_wad:
        wad_path = find_file(a.wad)
        if not wad_path:
            print(f"[!] WAD not found: {a.wad}")
            return 1
        print(f"[*] WAD      : {wad_path} "
              f"({os.path.getsize(wad_path):,} bytes)")

    local_ip = a.local_ip or get_local_ip()
    print(f"[*] Console IP : {a.host}")
    print(f"[*] iPhone IP  : {local_ip}")
    print(f"[*] doom_launcher.lua must have PC_IP = \"{local_ip}\"")

    log_thread = None
    if not a.no_log:
        log_thread = LogServer(a.log_port)
        log_thread.start()
        time.sleep(0.2)

    print()

    if not send_payload(a.host, a.launcher, a.payload_port):
        if log_thread:
            log_thread.stop()
        return 1

    time.sleep(1.0)

    if sc_path:
        if a.scport is not None:
            print(f"[sc] Using --scport {a.scport} (override)")
            sc_ok = stream_file(a.host, a.scport, sc_path, "sc",
                                connect_timeout=a.scport_wait)
        else:
            sc_ok = stream_shellcode(a.host, sc_path,
                                     total_timeout=a.scport_wait)
        if not sc_ok:
            if log_thread:
                log_thread.stop()
            return 1

    if wad_path and a.wad_delay > 0:
        print(f"[*] Waiting {a.wad_delay:.1f}s for shellcode to reach "
              f"WAD listener...")
        time.sleep(a.wad_delay)

    if wad_path:
        stream_file(a.host, a.wad_port, wad_path, "wad")

    if log_thread:
        print("\n[*] Watching logs — Ctrl-C to quit.")
        try:
            while log_thread.is_alive():
                time.sleep(0.5)
        except KeyboardInterrupt:
            print("\n[*] Stopping…")
        finally:
            log_thread.stop()
    print("Done!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
