# Guder Watch

Wearable readout for the Güder Lab K⁺ / pH biosensor, running on the
Waveshare **ESP32-S3-Touch-AMOLED-2.06**.

```
pH ─┐                                      ┌─ AMOLED + touch UI
    ├─ Sensor MCU ──UART──▶ ESP32-S3 watch ├─ calibration (Nernst fit)
K+ ─┘  (high-Z front end,                  ├─ K+ alert + vibration
        ADC, sends mV)                     ├─ CSV logging (SD / flash)
                                           ├─ BLE (live data, time sync)
                                           └─ USB console (status, log export)
```

The sensor MCU only measures and sends **raw millivolts**. Everything that
depends on the electrodes — calibration, concentration, alerts — lives on the
watch, so new electrodes never need new sensor-MCU firmware.

> **New here?** Start with [GUIDE.md](GUIDE.md): flashing, using and changing the watch, step by step.

> **Status:** firmware builds and the parser and calibration maths are unit-tested
> on the host. It has **not yet run on the watch hardware**.
> The sensor MCU and its analog front end don't exist yet. Use demo mode or
> `tools/sensor_mcu_emulator.py` until they do.

---

## Wiring the sensor MCU

Bottom header of the watch board (3.3 V logic only):

| Watch pad | GPIO | Connect to |
|---|---|---|
| `RXD` | 44 | sensor MCU **TX** |
| `TXD` | 43 | sensor MCU RX (unused for now) |
| `GND` | — | sensor MCU GND |

UART1, 115200 8N1, configurable in `idf.py menuconfig` → *Guder Watch*.
The ESP32's own console moved to the USB port, so these pins are free.

### Front-end requirement (important)

An ion-selective electrode is a voltage source with **very high internal
resistance** (MΩ–GΩ). The sensor MCU must buffer each electrode with an
amplifier whose input bias current is in the pA range, such as an electrometer-grade
op-amp follower, before its ADC. Feeding an electrode straight into an MCU
ADC pin, including the ESP32's, loads it and gives wrong, drifting readings.

## Wire protocol

One ASCII line per sample:

```
$GKS,<seq>,<t_ms>,<k_mV>,<ph_mV>,<temp_C>,<flags>*<CS>\r\n
$GKS,42,123456,152.250,-21.500,36.8,0*5C
```

| Field | Meaning |
|---|---|
| `seq` | frame counter. Gaps are counted as dropped frames |
| `t_ms` | sensor-MCU uptime |
| `k_mV`, `ph_mV` | electrode potential vs reference, in mV. **Empty** means channel not fitted |
| `temp_C` | optional, empty if not measured |
| `flags` | hex: `1` K⁺ fault, `2` pH fault, `4` ADC saturated, `8` low supply |
| `CS` | XOR of every byte between `$` and `*`, two hex digits |

The spec lives in `main/sensor_protocol.h`, and `tools/sensor_mcu_emulator.py` is
a reference implementation. Any MCU that can `printf` can send this. **Any
sample rate works.** The watch keeps history per second, so 1–10 Hz (sensible for ISEs)
and faster rates all give correct 5/30 min trends. Bluetooth sends at most 1 update/s.

## Build and flash

```bash
. ~/.espressif/v5.5.5/esp-idf/export.sh
```

```bash
idf.py build flash monitor
```

The monitor port is the board's USB port (e.g. `/dev/tty.usbmodem3101`).
At the `gw>` prompt type `help`.

## Using the watch

A splash screen shows at boot, then the watch face. **Swipe left and right** to move
between five pages. The dots at the bottom show which page you're on.

| Page | What it does |
|---|---|
| **Watch face** | Time and status (`LIVE` / `DEMO` / `LOST`, Bluetooth, storage, battery). K⁺ gauge with green (normal), amber (elevated) and red (≥ alert threshold) zones. The marker and number glide to each new value. pH ring, 30 min K⁺ sparkline and a trend arrow (e.g. `▲ +0.6 mM / 10 min`). The date line turns into a warning for "clock not set" or "movement". Tap the gauge or the pH ring to open Trends |
| **Trends** | K⁺ (with the alert threshold line) and pH over 5 or 30 min |
| **Calibrate** | Guided 2–5 point calibration, see below |
| **Diagnostics** | Link counters, raw mV, flags, IMU, battery, storage, clock and BLE state |
| **Settings** | Bluetooth, vibration, demo data, K⁺ alert threshold, brightness, set time, clear logs |

When K⁺ stays above the alert threshold for 5 s (so single spikes are ignored), a pulsing red full-screen alert appears over
whatever page is open, showing the value, its 10 min change and the threshold. It has
*See trend* and *Dismiss* buttons. Once dismissed, it only comes back when a new alert
starts, i.e. after K⁺ has dropped back below the threshold and risen again.

**Demo data** produces a realistic synthetic signal, including a K⁺ spike
every 4 min, so the whole pipeline can be shown without hardware. It is labelled
`DEMO` on screen, in the logs (`source=demo`) and over BLE. Real sensor data
always takes priority over demo data.

### Calibration workflow

1. Menu → Calibrate → pick **K+** or **pH**.
2. Put the electrodes in the first standard. Wait until the readout says
   **STABLE** (30 s window: SD ≤ 0.5 mV and drift ≤ 1 mV).
3. Select that standard in the roller and tap **Capture standard**.
4. Repeat for 2–5 standards. Re-capturing a standard replaces its point.
5. **Fit & save** shows slope, % of Nernstian, intercept and R², with a
   verdict ("healthy", "sub-Nernstian", "wrong sign", …). Save or discard.

Calibrations are kept across reboots and every save is appended to
`gw_cal.csv`. Calibration needs live sensor data and is disabled in
demo mode.

K⁺ standards offered: 0.1 / 1 / 10 / 100 mM. pH buffers: 4.01 / 6.86 / 7.00 / 9.18 / 10.01.
Concentration is used in place of activity, so make standards up in the same
background matrix as the samples (e.g. artificial synovial fluid / PBS).

## Logs

- **SD card inserted:** every sample goes to `/sdcard/gw_YYYYMMDD.csv`.
- **No card:** internal flash (`/spiffs`, ~7 MB), one averaged row every
  10 s (~1 MB/day). Logging stops with a warning below 256 KB free.
- Rows logged before the clock is set go to `gw_nodate.csv`.

Columns:
`local_time, epoch_ms, uptime_ms, source, seq, n, k_mV, k_mM, ph_mV, pH, temp_C, flags, accel_g, motion, battery_pct, k_alert, time_valid`

To copy the logs to the laptop (close `idf.py monitor` first):

```bash
python tools/pull_logs.py /dev/tty.usbmodem3101 --out data/
```

## Clock

The PCF85063 RTC keeps UTC. The display uses UK time (`GW_TIMEZONE`). If the RTC
lost power, the watch shows *clock not set* and starts from the firmware
build time. You can set it three ways:

- on the watch: Settings → Set time
- over USB: `settime 1792500000` (unix seconds, e.g. from `date +%s`)
- over BLE: write a uint32 LE unix time to the Time characteristic

## Bluetooth

The device appears as `GuderWatch` and works with any generic BLE app (e.g. nRF Connect).

| Characteristic | UUID | Access |
|---|---|---|
| Live reading (CSV text) | `6e7a0002-3c1f-4b8e-9d2a-5f4c1b2a7e90` | read, notify |
| Time, uint32 LE unix s | `6e7a0003-3c1f-4b8e-9d2a-5f4c1b2a7e90` | read, write |
| Battery level | `0x2A19` (Battery Service) | read, notify |

There is no pairing. That's fine on the bench, but add LE Secure Connections before
any work with people.

## Previewing the UI without the watch

```bash
./test/sim/run.sh
```

This compiles the real `main/ui/` code and LVGL for your Mac, feeds it fake readings,
and writes screenshots of every page to `test/sim/out/`. Scenarios: live, alert, demo,
uncalibrated, no sensor. Use it to check layout changes before flashing. Run
`idf.py build` once first so LVGL is downloaded.

## Tests

```bash
./test/host/run.sh
```

This runs 66 unit checks (frame parser, line resync, calibration maths, Nernst)
under ASan/UBSan, plus a contract test that feeds emulator output through the
firmware parser.

Testing against the real watch with a USB-UART adapter:

```bash
python tools/sensor_mcu_emulator.py /dev/tty.usbserial-XXXX --scenario spike
```

```bash
python tools/sensor_mcu_emulator.py /dev/tty.usbserial-XXXX --scenario standards
```

`spike` exercises the alert. `standards` steps through 1/10/100 mM and pH 4/7/10
every 90 s so you can practise calibration.

## Placeholders to revisit with the lab

| Item | Where | Current value |
|---|---|---|
| K⁺ alert threshold | Settings / Kconfig | 6.0 mM, a bench placeholder, **not** a clinical cut-off |
| Stability criteria for capture | `ui/ui_calib.c` | SD ≤ 0.5 mV, drift ≤ 1 mV over 30 s |
| Alert hold | `Kconfig.projbuild` (`GW_K_ALERT_HOLD_S`) | K⁺ must stay above the threshold for 5 s |
| Motion thresholds | `housekeeping.c` | \|a\| ±0.15 g or 40 °/s |
| "Normal" band on the gauge | `ui/ui_internal.h` | below 5.0 mM |
| Trend arrow thresholds | `ui/ui_face.c` | ±0.3 mM/10 min notable, +1.0 fast |
| Sample rate | front end | any; history is per second |
| Standards offered | `ui/ui_calib.c` | listed above |
| Temperature compensation | — | none yet; the slope % is reported against 25 °C |

## Code map

```
main/
  main.c              start-up order
  app_state.*         data hub: latest reading, history, settings, calibration (NVS)
  sensor_protocol.*   frame parser / formatter (host-testable, no IDF)
  calibration.*       linear fit, Nernst helpers (host-testable, no IDF)
  sensor_link.*       UART receiver task + demo generator
  rtc_service.*       PCF85063 clock
  data_logger.*       CSV logging task (SD or SPIFFS)
  ble_service.*       NimBLE GATT server
  haptics.*           vibration motor (GPIO18)
  housekeeping.*      IMU 10 Hz + battery 5 s polling
  usb_console.*       status / settime / ls / cat / rm
  imu_service.*       QMI8658 driver
  power_service.*     AXP2101 via XPowersLib
  ui/                 LVGL pages (face, trends, calibrate, diagnostics, settings, alert)
tools/                sensor-MCU emulator, log puller
test/host/            unit + contract tests
test/sim/             host UI renderer -> screenshots
```
