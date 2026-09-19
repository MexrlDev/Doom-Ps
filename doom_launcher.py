#!/usr/bin/env python3
"""
doom_launcher.py — Doom-ps launcher (iPhone / desktop).

"""

import argparse, datetime, os, platform, socket, struct, sys, threading, time

DEFAULT_PS5_IP    = "" # Luac0re IP here. the ip shown on screen after luac0re success
DEFAULT_LAUNCHER  = "doom_launcher.lua" # < lua name. .. bleh :p
DEFAULT_SHELLCODE = "doom_ps.bin"
DEFAULT_WAD       = "wad"           # folder, not file
PAYLOAD_PORT      = 9026 # < luac0re payload port on screen. already set hah
WAD_PORT          = 5000 # < boring boring. port for wad
LOG_PORT          = 9027 # < boring logs. im sleepy
SC_PORT_LO        = 5001
SC_PORT_HI        = 5021
CHUNK             = 64 * 1024

# Retry tuning
PAYLOAD_RETRIES       = 5
PAYLOAD_RETRY_DELAY   = 1.0
SHELLCODE_RETRIES     = 3
SHELLCODE_RETRY_DELAY = 1.5
WAD_CONNECT_RETRIES   = 5
WAD_CONNECT_DELAY     = 1.0
WAD_TRANSFER_RETRIES  = 1
WAD_INTER_DELAY       = 0.15

IS_WINDOWS = os.name == "nt"
OS_NAME    = platform.system() or "Unknown"


def find_file(name, subdirs=("payloads", "lua", "wad", ".")):
    if os.path.isfile(name):
        return name
    for sub in subdirs:
        cand = os.path.join(sub, os.path.basename(name))
        if os.path.isfile(cand):
            return cand
    return None


def find_wads(dir_or_file):
    """Return sorted list of .wad files.  Accepts a dir or a single file."""
    if os.path.isfile(dir_or_file):
        return [dir_or_file] if dir_or_file.lower().endswith(".wad") else []
    if not os.path.isdir(dir_or_file):
        return []
    out = []
    for f in sorted(os.listdir(dir_or_file)):
        if f.lower().endswith(".wad"):
            out.append(os.path.join(dir_or_file, f))
    return out


def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        try:
            hostname = socket.gethostname()
            return socket.gethostbyname(hostname)
        except OSError:
            return "127.0.0.1"
    finally:
        s.close()


def make_udp_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError:
        pass
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE") and IS_WINDOWS:
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 0)
        except OSError:
            pass
    return s


class LogServer(threading.Thread):
    def __init__(self, port, host="0.0.0.0"):
        super().__init__(daemon=True)
        self.host, self.port = host, port
        self._stop_event = threading.Event()
        self.sock = None

    def run(self):
        s = make_udp_socket()
        bound = False
        for attempt in range(3):
            try:
                s.bind((self.host, self.port))
                bound = True
                break
            except OSError as e:
                if attempt == 2:
                    print(f"[log] WARN: cannot bind UDP "
                          f"{self.host}:{self.port} ({e}).")
                    if IS_WINDOWS:
                        print("[log] Windows: allow Python through "
                              "Windows Defender Firewall for UDP "
                              f"{self.port}.")
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
        s.close()

    def stop(self):
        self._stop_event.set()
        if self.sock:
            try: self.sock.close()
            except OSError: pass


# ============================================================
# Retry-aware senders
# ============================================================

def send_payload(host, filepath, port, retries=PAYLOAD_RETRIES):
    """Send doom_launcher.lua to LuaC0re on port 9026.

    Retries the whole connect+send up to `retries` times with a short
    delay between attempts.  The LuaC0re loader sometimes drops the
    first connection when the game is still booting.
    """
    path = find_file(filepath)
    if not path:
        print(f"[!] Launcher not found: {filepath}")
        return False
    with open(path, "rb") as f:
        data = f.read()
    print(f"[1] Sending {os.path.basename(path)} "
          f"({len(data):,} bytes) -> {host}:{port}")

    last_err = None
    for attempt in range(1, retries + 1):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        try:
            s.connect((host, port))
            s.sendall(data)
            s.close()
            if attempt > 1:
                print(f"[1] Sent on attempt {attempt}")
            return True
        except Exception as e:
            last_err = e
            try: s.close()
            except OSError: pass
            if attempt < retries:
                print(f"[1] Attempt {attempt}/{retries} failed: {e}. "
                      f"Retrying in {PAYLOAD_RETRY_DELAY}s...")
                time.sleep(PAYLOAD_RETRY_DELAY)
    print(f"[!] Payload send failed after {retries} attempts: {last_err}")
    return False


def _send_shellcode_once(host, port, data, size):
    """One attempt to stream `data` to `host:port`.  Returns True on full send."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        s.connect((host, port))
    except (ConnectionRefusedError, OSError):
        s.close()
        return False

    print(f"[sc] Connected to {host}:{port}, sending {size:,} bytes")
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
        print(f"\n[!] Shellcode send broke at {sent:,}/{size:,}: {e}")
        try: s.close()
        except OSError: pass
        return False


def send_shellcode_to_port(host, port, path, retries=SHELLCODE_RETRIES):
    """Send raw shellcode bytes to an explicit port (override path)."""
    if not os.path.isfile(path):
        print(f"[!] Shellcode not found: {path}")
        return False
    with open(path, "rb") as f:
        data = f.read()
    size = len(data)

    for attempt in range(1, retries + 1):
        if attempt > 1:
            print(f"[sc] Retry {attempt}/{retries} in "
                  f"{SHELLCODE_RETRY_DELAY}s...")
            time.sleep(SHELLCODE_RETRY_DELAY)
        if _send_shellcode_once(host, port, data, size):
            return True
    print(f"[!] Shellcode failed after {retries} attempts")
    return False


def stream_shellcode(host, path,
                     port_lo=SC_PORT_LO, port_hi=SC_PORT_HI,
                     per_port_timeout=0.5, total_timeout=25,
                     retries=SHELLCODE_RETRIES):
    """Scan the SC port range and stream the shellcode, retrying the
    whole transfer up to `retries` times if it fails partway."""
    if not os.path.isfile(path):
        print(f"[!] Shellcode not found: {path}")
        return False
    with open(path, "rb") as f:
        data = f.read()
    size = len(data)

    for attempt in range(1, retries + 1):
        if attempt > 1:
            print(f"[sc] Retry {attempt}/{retries} in "
                  f"{SHELLCODE_RETRY_DELAY}s...")
            time.sleep(SHELLCODE_RETRY_DELAY)

        print(f"[sc] Scanning {host}:{port_lo}..{port_hi - 1} "
              f"for shellcode port")
        deadline = time.time() + total_timeout
        scan = 0
        found_and_sent = False

        while time.time() < deadline:
            scan += 1
            for port in range(port_lo, port_hi):
                if _send_shellcode_once(host, port, data, size):
                    found_and_sent = True
                    break
            if found_and_sent:
                return True
            if scan % 5 == 0:
                print(f"[sc]   ...no listener yet (attempt {attempt})")
            time.sleep(0.3)

        print(f"[sc] Port not found in {total_timeout}s "
              f"(attempt {attempt}/{retries})")

    print(f"[!] Shellcode failed after {retries} attempts")
    return False


def _stream_wad_once(host, port, path, connect_timeout=30):
    """One attempt.  Returns True on full send, False otherwise."""
    size = os.path.getsize(path)
    name = os.path.basename(path).encode("utf-8")[:63]

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
                print(f"[!] WAD connect timeout: {e}")
                return False
            time.sleep(0.5)
    s.settimeout(None)

    s.sendall(struct.pack("<QH", size, len(name)))
    s.sendall(name)

    sent, t0 = 0, time.time()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            s.sendall(chunk)
            sent += len(chunk)
            pct = sent * 100 // size
            print(f"\r[wad] {os.path.basename(path)} "
                  f"{sent:,}/{size:,} ({pct}%)", end="", flush=True)
    print()
    s.close()
    dt = max(time.time() - t0, 1e-6)
    print(f"[wad] {os.path.basename(path)} done in {dt:.1f}s "
          f"({sent / dt / 1024:.0f} KB/s)")
    return True


def stream_wad(host, port, path,
               connect_retries=WAD_CONNECT_RETRIES,
               transfer_retries=WAD_TRANSFER_RETRIES):
    """Stream one WAD, retrying the transfer if it fails.

    NOTE: if a WAD gets partially sent the console will delete every
    WAD it already received and show an error screen.  Retrying the
    whole WAD after that requires the user to press O on the console
    first — the retry here only helps when the console is still
    listening.
    """
    if not os.path.isfile(path):
        print(f"[!] WAD not found: {path}")
        return False

    for attempt in range(1, transfer_retries + 2):
        if attempt > 1:
            print(f"[wad] Retry {attempt - 1}/{transfer_retries} "
                  f"in {WAD_CONNECT_DELAY}s...")
            time.sleep(WAD_CONNECT_DELAY)
        if _stream_wad_once(host, port, path):
            return True
    print(f"[!] WAD failed: {os.path.basename(path)}")
    return False


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
    ap.add_argument("--scport",       type=int, default=None)
    ap.add_argument("--scport-wait",  type=int, default=25)
    ap.add_argument("--wad-delay",    type=float, default=2.0)
    a = ap.parse_args()

    print("=" * 60)
    print(" Doom-ps launcher — multi-WAD")
    print(f" Host OS: {OS_NAME}")
    print("=" * 60)

    sc_path = None
    wads = []
    if not a.no_shellcode:
        sc_path = find_file(a.shellcode)
        if not sc_path:
            print(f"[!] Shellcode not found: {a.shellcode}")
            return 1
        print(f"[*] Shellcode: {sc_path} "
              f"({os.path.getsize(sc_path):,} bytes)")
    if not a.no_wad:
        wads = find_wads(a.wad)
        if not wads:
            print(f"[!] No .wad files found at {a.wad}")
            return 1
        print(f"[*] Found {len(wads)} WAD(s):")
        for w in wads:
            print(f"      {os.path.basename(w)}")

    local_ip = a.local_ip or get_local_ip()
    print(f"[*] Console IP : {a.host}")
    print(f"[*] Host IP    : {local_ip}")
    print(f"[*] doom_launcher.lua must have PC_IP = \"{local_ip}\"")
    if IS_WINDOWS and local_ip == "127.0.0.1":
        print("[*] Windows: no default route detected — pass --local-ip "
              "with your LAN address.")

    log_thread = None
    if not a.no_log:
        log_thread = LogServer(a.log_port)
        log_thread.start()
        time.sleep(0.2)

    print()

    if not send_payload(a.host, a.launcher, a.payload_port):
        if log_thread: log_thread.stop()
        return 1

    time.sleep(1.0)

    if sc_path:
        if a.scport is not None:
            print(f"[sc] Using --scport {a.scport} (override)")
            ok = send_shellcode_to_port(a.host, a.scport, sc_path)
        else:
            ok = stream_shellcode(a.host, sc_path,
                                  total_timeout=a.scport_wait)
        if not ok:
            if log_thread: log_thread.stop()
            return 1

    if wads:
        if a.wad_delay > 0:
            print(f"[*] Waiting {a.wad_delay:.1f}s for WAD listener...")
            time.sleep(a.wad_delay)

        sent_ok = 0
        for i, w in enumerate(wads):
            if stream_wad(a.host, a.wad_port, w):
                sent_ok += 1
            if i < len(wads) - 1:
                time.sleep(WAD_INTER_DELAY)
        print(f"[*] WADs sent: {sent_ok}/{len(wads)}")

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
