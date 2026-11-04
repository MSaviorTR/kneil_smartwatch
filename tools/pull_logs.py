#!/usr/bin/env python3
"""
Copies the watch's CSV logs to this computer over the USB cable.

    python tools/pull_logs.py /dev/tty.usbmodem3101            # all files -> ./watch_logs
    python tools/pull_logs.py PORT --out data/day1 --only gw_20261020.csv

Uses the watch's USB console (`ls` / `cat`). Close `idf.py monitor`
first - only one program can hold the port.
"""

import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")

PROMPT = b"gw>"


def read_until(port, marker: bytes, timeout: float) -> bytes:
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            buf += chunk
            deadline = time.monotonic() + timeout   # still streaming
            if marker in buf:
                return buf
    raise TimeoutError(f"timed out waiting for {marker!r}")


def command(port, cmd: str, end: bytes, timeout: float = 5.0) -> bytes:
    port.reset_input_buffer()
    port.write(cmd.encode() + b"\n")
    return read_until(port, end, timeout)


def list_files(port):
    out = command(port, "ls", PROMPT).decode(errors="replace")
    files = []
    for line in out.splitlines():
        m = re.match(r"\s*(\d+)\s+(\S+\.csv)\s*$", line)
        if m:
            files.append((m.group(2), int(m.group(1))))
    return files


def fetch(port, name: str) -> bytes:
    raw = command(port, f"cat {name}", b"-----END-----", timeout=10.0)
    m = re.search(rb"-----BEGIN (\S+) (-?\d+)-----\r?\n(.*?)-----END-----", raw, re.S)
    if not m:
        raise RuntimeError(f"no BEGIN/END block for {name}")
    data = m.group(3)
    # The USB-JTAG console turns \n into \r\n on output; undo that.
    return data.replace(b"\r\n", b"\n")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("--out", default="watch_logs")
    p.add_argument("--only", help="copy just this file")
    args = p.parse_args()

    os.makedirs(args.out, exist_ok=True)

    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        port.write(b"\n")
        time.sleep(0.3)

        files = [(args.only, -1)] if args.only else list_files(port)
        if not files:
            print("no log files on the watch")
            return

        for name, size in files:
            data = fetch(port, name)
            dest = os.path.join(args.out, name)
            with open(dest, "wb") as f:
                f.write(data)
            note = "" if size < 0 or len(data) == size else f"  (watch said {size} B - re-run if it was still being written)"
            print(f"{name:24s} {len(data):9d} B -> {dest}{note}")


if __name__ == "__main__":
    main()
