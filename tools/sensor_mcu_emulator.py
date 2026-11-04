#!/usr/bin/env python3
"""
Pretends to be the sensor MCU: sends $GKS frames to the watch over a
3.3 V USB-UART adapter, so the full link can be tested before the real
front end exists.

Wiring (3.3 V logic only - a 5 V adapter can damage the ESP32):
    adapter TX  -> watch pad "RXD" (GPIO44)
    adapter GND -> watch pad "GND"

Examples:
    python tools/sensor_mcu_emulator.py /dev/cu.usbmodem3101 --usb  # over the watch's USB cable
    python tools/sensor_mcu_emulator.py /dev/tty.usbserial-0001
    python tools/sensor_mcu_emulator.py PORT --scenario spike
    python tools/sensor_mcu_emulator.py PORT --scenario standards   # practise calibration
    python tools/sensor_mcu_emulator.py --stdout --count 5          # just print frames

Scenarios:
    steady     K+ ~4.2 mM, pH ~7.4 with noise and slow drift
    spike      steady, plus a lysis-like K+ rise to ~9 mM every 3 min
    standards  K+ steps 1 -> 10 -> 100 mM and pH 4 -> 7 -> 10 every
               90 s, mimicking moving the electrodes between standards
"""

import argparse
import math
import random
import sys
import time

# Electrode model used to turn concentrations into potentials. Different
# from the watch's demo calibration on purpose, so calibrating against
# the emulator gives a non-trivial result.
K_SLOPE, K_E0 = 57.2, 108.0        # mV/decade, mV at 1 mM
PH_SLOPE, PH_E0 = -56.4, 402.0     # mV/pH,     mV at pH 0
NOISE_MV = 0.25
SETTLE_S = 12.0                    # electrode response time constant


def checksum(body: str) -> int:
    cs = 0
    for ch in body.encode("ascii"):
        cs ^= ch
    return cs


def frame(seq, t_ms, k_mv, ph_mv, temp_c, flags=0, bad_checksum=False):
    def f(v):
        return "" if v is None else f"{v:.3f}"

    body = f"GKS,{seq},{t_ms},{f(k_mv)},{f(ph_mv)},{f(temp_c)},{flags:X}"
    cs = checksum(body) ^ (0x5A if bad_checksum else 0)
    return f"${body}*{cs:02X}\r\n"


class Scenario:
    def __init__(self, name):
        self.name = name
        self.k_mv = K_E0 + K_SLOPE * math.log10(4.2)
        self.ph_mv = PH_E0 + PH_SLOPE * 7.4

    def targets(self, t):
        if self.name == "standards":
            step = int(t // 90) % 3
            k = (1.0, 10.0, 100.0)[step]
            ph = (4.0, 7.0, 10.0)[step]
            return k, ph

        k = 4.2 + 0.3 * math.sin(t / 90.0)
        ph = 7.40 - 0.04 * math.sin(t / 120.0)

        if self.name == "spike":
            phase = t % 180.0
            if phase > 60.0:
                since = phase - 60.0
                rise = since / 20.0 if since < 20.0 else math.exp(-(since - 20.0) / 30.0)
                k += 5.0 * rise
                ph -= 0.15 * rise   # infection tends to acidify locally

        return k, ph

    def step(self, t, dt):
        k, ph = self.targets(t)
        k_target = K_E0 + K_SLOPE * math.log10(k)
        ph_target = PH_E0 + PH_SLOPE * ph

        # First-order electrode response, so step changes "settle".
        a = 1.0 - math.exp(-dt / SETTLE_S)
        self.k_mv += (k_target - self.k_mv) * a
        self.ph_mv += (ph_target - self.ph_mv) * a

        return (self.k_mv + random.gauss(0, NOISE_MV),
                self.ph_mv + random.gauss(0, NOISE_MV),
                36.5 + random.gauss(0, 0.05))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="serial port of the USB-UART adapter")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--rate", type=float, default=1.0, help="frames per second (default 1)")
    p.add_argument("--scenario", choices=["steady", "spike", "standards"], default="steady")
    p.add_argument("--fault", choices=["none", "k", "ph"], default="none",
                   help="set a channel fault flag on every frame")
    p.add_argument("--corrupt", type=float, default=0.0,
                   help="fraction of frames sent with a bad checksum (link-robustness test)")
    p.add_argument("--usb", action="store_true",
                   help="PORT is the watch's own USB port: send frames as console "
                        "'frame' commands, no USB-UART adapter needed")
    p.add_argument("--stdout", action="store_true", help="print frames instead of using a port")
    p.add_argument("--count", type=int, default=0, help="stop after N frames (0 = forever)")
    args = p.parse_args()

    if not args.stdout and not args.port:
        p.error("give a serial port, or --stdout")

    out = None
    if not args.stdout:
        try:
            import serial  # pyserial (bundled with the ESP-IDF Python env)
        except ImportError:
            sys.exit("pyserial missing: pip install pyserial")
        out = serial.Serial(args.port, args.baud, timeout=0)

    flags = {"none": 0, "k": 0x01, "ph": 0x02}[args.fault]
    sim = Scenario(args.scenario)
    period = 1.0 / args.rate
    start = time.monotonic()
    seq = 0

    try:
        while args.count == 0 or seq < args.count:
            t = time.monotonic() - start
            k_mv, ph_mv, temp = sim.step(t, period)
            bad = args.corrupt > 0 and random.random() < args.corrupt
            line = frame(seq, int(t * 1000), k_mv, ph_mv, temp, flags, bad_checksum=bad)

            if out is None:
                sys.stdout.write(line)
                sys.stdout.flush()
            else:
                out.write((("frame " + line.strip() + "\n") if args.usb else line).encode("ascii"))
                out.read(4096)   # drain the console echo so buffers never fill
                if seq % 10 == 0:
                    print(f"[{t:7.1f}s] seq {seq}  K {k_mv:7.2f} mV  pH {ph_mv:7.2f} mV")

            seq += 1
            time.sleep(max(0.0, start + seq * period - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        if out is not None:
            out.close()


if __name__ == "__main__":
    main()
