# Project context — Guder Watch

Handoff summary: the *why* and the current state, which the code alone does not show.
For wiring, protocol, build steps and usage, see [README.md](README.md).
For the plan to get real biosensor data onto the watch, see [ROADMAP.md](ROADMAP.md).

*Last updated: 5 Nov 2026*

---

## 1. What this is for

- **Internship:** EPSRC Vacation Internship, Güder Lab, Dept. of Bioengineering, Imperial
  College London (summer 2026). Supervisor Prof. Firat Güder; day-to-day supervisor
  Muhammed Mehdi Menteş.
- **Research goal:** a potassium ion-selective electrode (K⁺ ISE) to detect infection or cell
  death around implants, e.g. periprosthetic joint infection. When cells or bacteria lyse they
  release intracellular K⁺, so a local rise in K⁺ could flag infection early. pH is a second
  signal, since local acidosis can accompany infection.
- **This repo:** firmware for an **ESP32-S3 smartwatch** (Waveshare ESP32-S3-Touch-AMOLED-2.06)
  that acts as the readout. It displays, calibrates, alerts on, logs and transmits the electrode
  measurements.
- **Most of the wider project is still undecided:** electrode materials, reference electrode,
  measurement hardware and final form factor. Treat the architecture below as the current best
  guess.

## 2. Architecture

```
pH electrode ─┐
K+ electrode ─┼─▶ [measurement front end]  ──frames──▶  ESP32-S3 watch
reference ────┘    (not built yet, see §6)              ├─ calibration (Nernst fit)
                                                        ├─ display + K+ alert + vibration
                                                        ├─ CSV logging (SD / flash)
                                                        ├─ BLE (live data, time, battery)
                                                        └─ USB console (status, logs, live frames)
```

**Key design decision:** the front end sends **raw millivolts**, never concentrations.
Calibration lives on the watch, so electrodes can change without new sensor firmware.

**Software design:** independent services (FreeRTOS tasks) around one data hub (`app_state`).
Producers (sensor link, demo generator, IMU/battery polling) push data in. Consumers (UI, logger,
BLE, haptics) read snapshots or receive callbacks. The UI never touches hardware directly.

## 3. Decisions made and why

| Decision | Reason |
|---|---|
| Project moved out of `~/Documents` to `~/Projects/ESP32/guder_watch` | Documents and Desktop are iCloud-synced. iCloud created `… 2` duplicate folders that break ESP-IDF builds |
| Raw mV over the wire; calibration on the watch | Electrodes will change often during development |
| ASCII frames with an XOR checksum (`$GKS,…*CS`) | Any microcontroller can `printf` them, easy to debug in a terminal, corrupted frames are rejected |
| Sensor link on UART1 using header pads RXD (GPIO44) / TXD (GPIO43); console moved to USB | These are the only spare pins on the header besides I2C and USB |
| Same frames also accepted over USB (`frame` console command) | Real data can stream from a laptop before any front-end hardware exists |
| Linear least-squares fit, 2–5 standards, reporting slope % and R² | Standard ISE practice; slope % (vs 59.16 mV/decade) is the usual electrode-health check |
| Concentration used in place of activity | Acceptable if standards share the samples' ionic background |
| K⁺ alert with hysteresis (0.3 mM) | Prevents the alert flickering when readings sit near the threshold |
| Demo mode, clearly labelled DEMO everywhere | Lets the whole system be demonstrated without sensors, with no risk of being mistaken for data |
| Swipe navigation, dial-style watch face, full-screen alert | Readable at a glance on a wrist: zone colours show the state before the number is read |
| LVGL draw buffer reduced to 40 lines; 64 KB internal RAM reserved | Found on first hardware run: screen updates failed once Bluetooth was added (see §5) |
| History stored per **second** as raw mV, converted on read | Charts, trend rate and the 30 min window stay correct at any front-end sample rate. Gaps show as gaps. Calibrating after a run fixes the past 30 min immediately |
| K⁺ alert needs 5 s above threshold (`GW_K_ALERT_HOLD_S`) | Real electrodes spike (cable movement, ADC glitches), so a single sample must not buzz the wrist |
| Saturated or faulted samples are never converted or averaged | A clipped ADC value would otherwise become a false concentration or alert |
| BLE notifications capped at 1 per second | A 10–100 Hz front end would otherwise flood the link |
| BLE central added alongside peripheral (17 Nov 2026), switchable Phone / Sensor / Both | The lab plans a **wireless sensor**: the watch must connect to it (central, data in) while still serving a phone (peripheral, data out). Wireless frames reuse the `$GKS` format and enter `app_state_push_frame()` like the wired link |
| Private GitHub repo, commits under the GitHub noreply email | Backup and syncing to the claude.ai project; avoids exposing the Mac's hostname |

## 4. Hardware facts (from the schematic and testing)

- **MCU:** ESP32-S3R8, 8 MB PSRAM, 16 MB flash (the flash chip reports 32 MB; the image uses 16).
  **Display:** 410×502 AMOLED (SH8601, QSPI), FT3168 touch.
- **Shared I2C bus** on GPIO15 (SDA) / GPIO14 (SCL), **broken out on the header** as IO15/IO14.
  Addresses already in use:
  - 0x18 ES8311 codec
  - 0x34 AXP2101 power IC
  - 0x38 touch
  - 0x40 ES7210 audio ADC
  - 0x51 PCF85063 RTC
  - 0x6B QMI8658 IMU
  - **0x48–0x4B are free** (ADS1115 range)
- **Bottom header:** VBUS, GND, D+/IO20, D−/IO19, IO15, IO14, RXD (GPIO44), TXD (GPIO43), GND, 3V3.
- **Other pins:** vibration motor on GPIO18 (via transistor); microSD in 1-bit SDMMC mode
  (CLK 2, CMD 1, D0 3); BOOT and PWR buttons on the side.

## 5. Status

### Tested on the real watch (30 Oct 2026)
- Boots to the splash screen, then the watch face; no screen errors after the memory fix.
- RTC keeps time across restarts (set from the Mac with `settime`).
- AXP2101 battery reading works (75–76 %). QMI8658 IMU starts.
- Logging works: no SD card, so it uses internal flash and writes `gw_YYYYMMDD.csv`.
- BLE advertises as `GuderWatch` (not yet tested from a phone).
- Demo data flows through the full pipeline.
- **Live data over USB:** 20/20 frames received, link `UP`, source `sensor`, readings shown
  UNCAL as expected because no calibration is stored yet.

### Bug found only on hardware
- The board package puts an 82 KB LVGL buffer in PSRAM, so each SPI flush needs an equal-size
  DMA bounce buffer in internal RAM. After BLE was added, that allocation failed intermittently
  (`Failed to allocate priv TX buffer`). Fixed with a 40-line buffer plus a 64 KB internal
  reserve. `status` now reports internal DMA headroom: about 62 KB largest free block vs 33 KB needed.

### Not yet verified
- Touch, swipe feel, animation smoothness and colours on the real screen (the user has not
  reported back yet).
- The alert overlay and vibration on hardware; BLE from a phone; SD card logging.
- **No real electrode data yet.**

### Code review for real sensor data (5 Nov 2026)
Fixed problems that demo data hid:
- **Sample-rate assumption.** History, trends, sparkline and the 10 min rate counted samples as
  seconds, so at 10 Hz "30 min" was 3 min. Link gaps were squashed out of charts.
  History is now per second.
- **Saturation flag ignored.** Clipped readings were converted to concentrations.
- **No alert debouncing.** One spike could trigger the alert and vibration.
- **Calibration did not apply to the past.** History now stores mV and converts on read.
- **BLE flooding at high rates**, and silently dropped log rows (now counted, shown in `status`).

Checked and fine: frame parsing and resync, sequence and drop counting (including sensor
reboots), the demo/real separation, locking between tasks, NVS versioning, and the logger's
full-disk handling. Every source (UART, USB `frame`, a future I2C ADC) goes through the single
entry point `app_state_push_frame()`, so adding a front end needs no changes elsewhere.

The ESP-IDF clang-tidy run could not analyse the code: it needs the clang toolchain, and this
project builds with GCC. Coverage comes instead from GCC `-Wall -Wextra` (clean) and host unit
tests under ASan/UBSan.

### Verified on the computer
- 66 unit checks (parser, calibration, Nernst) under ASan/UBSan.
- 30 checks on the data hub (`app_state`):
  - 10 Hz input still gives 30 min of history
  - gaps stay gaps
  - saturation is excluded
  - alert hold and hysteresis
  - re-calibration applies to the past
  - demo/real separation and drop counting
- Python ↔ C protocol contract test.
- UTC date maths checked against the system's `timegm`.
- UI renderer screenshots of every page and state: `./test/sim/run.sh`.

## 6. Getting live data — the main open problem

| Route | How | Status |
|---|---|---|
| **A. Lab instrument → laptop → USB → watch** | The potentiostat (OCP / potentiometry) or a high-impedance meter measures the electrodes. `tools/live_bridge.py` follows its output file or stdin and streams frames to the watch | **Watch side built and tested.** Needs the instrument make/model and how its software saves data. If it only exports after the run, write a direct driver instead (e.g. PalmSens EmStat over USB) |
| **B. Front end plugged into the watch** | Electrometer-grade buffers (e.g. LMP7721 or OPA320, fA–pA bias, 3.3 V) feeding an ADS1115 (16-bit) or ADS1220 (24-bit) on the header I2C pads; reference electrode biased at mid-supply (~1.65 V) | **Not started.** Needs parts and the supervisor's input; then an I2C ADC driver feeding `app_state` |
| **C. Separate sensor MCU over UART** | The original plan | Watch side ready; only worth it if the electronics must sit away from the wrist |
| **D. Wireless sensor over BLE** | The sensor board advertises the Guder Sensor service and notifies `$GKS` frames; the watch is the BLE central | **Watch side built** (Nov 2026); tested only by build and host tests so far. `tools/ble_sensor_emulator.py` turns the Mac into a test sensor |

**Why a buffer is mandatory:** ISEs have MΩ–GΩ source impedance. Connecting one straight to an ADC
(including the ESP32's) loads it and gives wrong, drifting readings.

## 7. Placeholders to replace with lab values

| Item | Where | Current value |
|---|---|---|
| K⁺ alert threshold | Settings / `Kconfig.projbuild` | 6.0 mM, **not** a clinical cut-off |
| Alert hold time | `Kconfig.projbuild` (`GW_K_ALERT_HOLD_S`) | 5 s above threshold before alerting |
| "Normal" band on the gauge | `ui/ui_internal.h` | below 5.0 mM |
| Trend arrow thresholds | `ui/ui_face.c` | ±0.3 mM/10 min notable, +1.0 fast |
| Calibration stability rule | `ui/ui_calib.c` | SD ≤ 0.5 mV and drift ≤ 1 mV over 30 s |
| Standards offered | `ui/ui_calib.c` | K⁺ 0.1/1/10/100 mM; pH 4.01/6.86/7.00/9.18/10.01 |
| Motion flag thresholds | `housekeeping.c` | \|a\| deviation 0.15 g or 40 °/s |
| Sample rate | front end | any rate works; history and charts are per second (1–10 Hz is sensible for ISEs) |
| Temperature compensation | — | none yet |

## 8. Open questions for the lab / supervisor

1. Which instrument measures the electrodes (make/model), and does its software write data **during** a run?
2. State of the electrodes: working K⁺ ISE and reference electrode yet?
3. Is a watch-mounted front end (route B) wanted? Preferred parts, or someone in the lab who does electronics?
4. What would a convincing demo be? A beaker titration with KCl additions, or wearing it?
5. Realistic values for the threshold, normal range and standards.
6. Is the wrist the right place? Implant-site K⁺ is measured far from the wrist, so the watch may only be a reader or demo platform.

## 9. Known limitations

- Bluetooth has no pairing or encryption: anyone nearby can read data or set the clock.
  Fine for the bench, not for use with people.
- Not a medical device; alerts are not clinically meaningful.
- No sleep or low-power mode, so battery life is "always awake".
- No over-the-air updates.
- On-watch history is 30 min; the full record is in the CSV logs.
- The wrist IMU only approximates electrode motion if the electrodes are elsewhere on the body.

## 10. Working on it

```bash
. ~/.espressif/v5.5.5/esp-idf/export.sh
```

```bash
idf.py build flash monitor
```

```bash
./test/host/run.sh
```

```bash
./test/sim/run.sh
```

- **Build, flash, monitor:** the 🔥 button in the VS Code ESP-IDF extension does the same as
  `idf.py build flash monitor`.
- **Tests:** `./test/host/run.sh` runs the unit and contract tests.
- **UI screenshots:** `./test/sim/run.sh` writes them to `test/sim/out/`.
- **Live data:** `python tools/live_bridge.py <port> --usb --follow <file> --k-col N [--volts]`.
- **Fake data:** `python tools/sensor_mcu_emulator.py <port> --usb --scenario spike|standards`.
- **Logs:** `python tools/pull_logs.py <port>`.
- **Watch USB port:** usually `/dev/cu.usbmodem3101`. Allow the "Espressif USB JTAG" accessory
  prompt in macOS the first time.
- **If flashing fails:** hold BOOT, press PWR, release BOOT, then retry.

**Where the code came from:** `imu_service.c` and `power_service.cpp` were written by Korhan in
Aug 2026 on top of the Waveshare `02_lvgl_demo_v9` example. Everything else was developed with
Claude Code (AI assistant) in Oct–Nov 2026, directed and tested on hardware by Korhan.
