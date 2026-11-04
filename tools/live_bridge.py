#!/usr/bin/env python3
"""
Streams REAL electrode measurements from the laptop to the watch.

Use this until the watch has its own measurement front end: whatever is
measuring the electrodes on the bench (potentiostat in OCP / potentiometry
mode, data-logging voltmeter, Arduino + ADC...) writes numbers on the
laptop, and this script forwards each new value to the watch as a
$GKS frame. The watch then calibrates, displays, alerts and logs them
exactly as it will with the final hardware.

Sources
  --follow FILE   watch a CSV/TXT file that the instrument software keeps
                  appending to (like `tail -f`); one row per sample
  --stdin         read values from the keyboard or a pipe, one row per line

Columns are picked by 0-based index or by header name:
  --k-col 1 --ph-col 2          or     --k-col "Potential K" --ph-col pH_V
Values are taken as millivolts; add --volts if the file is in volts.

Destination
  PORT --usb      the watch's own USB port (/dev/cu.usbmodem...)
  PORT            a USB-UART adapter wired to the watch's RXD pad

Examples
  # potentiostat software exporting time,E_K(V) live to a file
  python tools/live_bridge.py /dev/cu.usbmodem3101 --usb \\
         --follow ~/Desktop/ocp_run.csv --k-col 1 --volts

  # type potentials by hand (mV), e.g. reading a meter during calibration
  python tools/live_bridge.py /dev/cu.usbmodem3101 --usb --stdin --k-col 0

Close `idf.py monitor` first: only one program can hold the port.
"""

import argparse
import csv
import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sensor_mcu_emulator import frame  # one implementation of the wire format


def pick(row, header, col):
    """Value of column `col` (index or header name) in `row`, or None."""
    if col is None:
        return None
    if isinstance(col, str) and not col.lstrip("-").isdigit():
        if header is None or col not in header:
            sys.exit(f"column {col!r} not in header {header}")
        col = header.index(col)
    i = int(col)
    if i >= len(row):
        return None
    cell = row[i].strip()
    if cell == "":
        return None
    try:
        return float(cell)
    except ValueError:
        return None


def split(line):
    """Accepts comma, semicolon, tab or whitespace separated rows."""
    for d in (",", ";", "\t"):
        if d in line:
            return next(csv.reader(io.StringIO(line), delimiter=d))
    return line.split()


def follow(path):
    """Yields lines appended to `path` (starting from the end, like tail -f)."""
    with open(path, "r", errors="replace") as f:
        header_line = f.readline()
        f.seek(0, os.SEEK_END)
        yield ("header", header_line)
        partial = ""
        while True:
            chunk = f.readline()
            if not chunk:
                time.sleep(0.2)
                continue
            partial += chunk
            if partial.endswith("\n"):
                yield ("row", partial)
                partial = ""


def stdin_lines():
    for line in sys.stdin:
        yield ("row", line)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="watch USB port (with --usb) or USB-UART adapter port")
    p.add_argument("--usb", action="store_true", help="send over the watch's USB console")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--follow", metavar="FILE")
    src.add_argument("--stdin", action="store_true")
    p.add_argument("--k-col", help="column with the K+ electrode potential")
    p.add_argument("--ph-col", help="column with the pH electrode potential")
    p.add_argument("--temp-col", help="optional temperature column (deg C)")
    p.add_argument("--volts", action="store_true", help="input is in V, convert to mV")
    p.add_argument("--baud", type=int, default=115200)
    args = p.parse_args()

    if args.k_col is None and args.ph_col is None:
        p.error("give --k-col and/or --ph-col")

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing: pip install pyserial")

    out = serial.Serial(args.port, args.baud, timeout=0)
    scale = 1000.0 if args.volts else 1.0
    header = None
    seq = 0
    start = time.monotonic()

    source = follow(os.path.expanduser(args.follow)) if args.follow else stdin_lines()
    print("bridging - Ctrl+C to stop", file=sys.stderr)

    try:
        for kind, line in source:
            cells = split(line.strip())

            if kind == "header":
                # Only treat it as a header if it is not numeric data.
                if cells and pick(cells, None, 0) is None:
                    header = [c.strip() for c in cells]
                continue

            k = pick(cells, header, args.k_col)
            ph = pick(cells, header, args.ph_col)
            temp = pick(cells, header, args.temp_col)

            if k is None and ph is None:
                continue   # header repeats, comments, blank lines

            k_mv = k * scale if k is not None else None
            ph_mv = ph * scale if ph is not None else None
            t_ms = int((time.monotonic() - start) * 1000)
            line_out = frame(seq, t_ms, k_mv, ph_mv, temp).strip()

            out.write(((f"frame {line_out}\n") if args.usb else line_out + "\r\n").encode("ascii"))
            out.read(4096)

            print(f"seq {seq:5d}  K {'-' if k_mv is None else f'{k_mv:8.2f}'} mV"
                  f"  pH {'-' if ph_mv is None else f'{ph_mv:8.2f}'} mV", file=sys.stderr)
            seq += 1
    except KeyboardInterrupt:
        pass
    finally:
        out.close()


if __name__ == "__main__":
    main()
