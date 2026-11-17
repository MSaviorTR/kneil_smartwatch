#!/usr/bin/env python3
"""
Pretends to be a WIRELESS Guder sensor: the Mac becomes a BLE peripheral that
advertises the Guder Sensor service and notifies $GKS frames, so the watch's
central mode can be tested without sensor hardware.

The watch (Bluetooth mode "Sensor only" or "Phone + sensor") finds it,
connects, subscribes, and shows the data as LIVE.

Needs the `bless` package (BLE peripheral support for macOS/Linux/Windows):
    python -m pip install bless
The first run asks for Bluetooth permission for Terminal/your Python app.

Examples
    python tools/ble_sensor_emulator.py                    # steady K+ ~4.2 mM
    python tools/ble_sensor_emulator.py --scenario spike   # triggers the alert
    python tools/ble_sensor_emulator.py --rate 5           # 5 frames per second

What a real wireless sensor must implement (same contract):
    service         6e7a0010-3c1f-4b8e-9d2a-5f4c1b2a7e90  (advertised)
    characteristic  6e7a0011-3c1f-4b8e-9d2a-5f4c1b2a7e90  (NOTIFY)
    payload         one $GKS frame per notification (see main/sensor_protocol.h)
"""

import argparse
import asyncio
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sensor_mcu_emulator import Scenario, frame  # same physics + wire format

SERVICE_UUID = "6e7a0010-3c1f-4b8e-9d2a-5f4c1b2a7e90"
FRAMES_UUID = "6e7a0011-3c1f-4b8e-9d2a-5f4c1b2a7e90"


async def run(args):
    try:
        from bless import (BlessServer, GATTAttributePermissions,
                           GATTCharacteristicProperties)
    except ImportError:
        sys.exit("bless missing: python -m pip install bless")

    server = BlessServer(name=args.name, loop=asyncio.get_running_loop())
    await server.add_new_service(SERVICE_UUID)
    await server.add_new_characteristic(
        SERVICE_UUID, FRAMES_UUID,
        GATTCharacteristicProperties.read | GATTCharacteristicProperties.notify,
        None, GATTAttributePermissions.readable)
    await server.start()
    print(f'advertising as "{args.name}" - set the watch to Bluetooth mode '
          f'"Sensor only" or "Phone + sensor". Ctrl+C to stop.')

    sim = Scenario(args.scenario)
    period = 1.0 / args.rate
    start = time.monotonic()
    seq = 0

    try:
        while args.count == 0 or seq < args.count:
            t = time.monotonic() - start
            k_mv, ph_mv, temp = sim.step(t, period)
            payload = frame(seq, int(t * 1000), k_mv, ph_mv, temp).strip()

            server.get_characteristic(FRAMES_UUID).value = bytearray(payload, "ascii")
            server.update_value(SERVICE_UUID, FRAMES_UUID)

            if seq % 10 == 0:
                print(f"[{t:7.1f}s] seq {seq}  K {k_mv:7.2f} mV  pH {ph_mv:7.2f} mV")

            seq += 1
            await asyncio.sleep(max(0.0, start + seq * period - time.monotonic()))
    finally:
        await server.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", default="GuderSensor-Mac", help="advertised name")
    p.add_argument("--rate", type=float, default=1.0, help="frames per second")
    p.add_argument("--scenario", choices=["steady", "spike", "standards"], default="steady")
    p.add_argument("--count", type=int, default=0, help="stop after N frames (0 = forever)")
    args = p.parse_args()

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
