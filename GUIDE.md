# Guder Watch — your own guide

How to flash, use, change and understand the watch firmware without help.
Read Part 1 first; the rest when you need it.

---

## Part 1 — Put the firmware on the watch

### One-time checks
- The project folder is `~/Projects/ESP32/guder_watch`. Open it in VS Code: **File → Open Folder…**.
- If VS Code shows **Restricted Mode**, click it → **Trust**.
- The blue bar at the bottom should show **ESP-IDF v5.5.5** and the chip **esp32s3**. If the chip
  says `esp32`, click it → pick **esp32s3** → "via builtin USB-JTAG".

### Every time
1. **Plug in the watch** with a USB-C **data** cable. Press **PWR** if the screen is dark.
   - If macOS asks *"Allow accessory to connect? Espressif USB JTAG…"* → **Allow**.
2. **Choose the port:** click the 🔌 port item in the bottom bar → pick `/dev/cu.usbmodem…`
   (or `/dev/tty.usbmodem…`).
3. **Click the 🔥 flame** (*Build, Flash and Monitor*). If asked for a flash method → **UART**.
4. A terminal opens at the bottom:
   - **Build:** about 30 s the first time, seconds after that.
   - **Flash:** a % counter, then `Hash of data verified`.
   - **Monitor:** the watch's live log. Look for `main: up - type 'help' on this console`.
5. The watch shows the **GUDER WATCH** splash, then the watch face. ✅
6. **To stop the monitor:** click in the terminal and press **Ctrl + ]**.

### Same thing from Terminal (if VS Code misbehaves)

```bash
cd ~/Projects/ESP32/guder_watch && . ~/.espressif/v5.5.5/esp-idf/export.sh && idf.py flash monitor
```

### If something goes wrong

| What you see | What to do |
|---|---|
| Port list is empty / "no serial port" | Different cable (charge-only cables are common) or USB port; press PWR; allow the macOS accessory prompt |
| `Failed to connect` / `No serial data received` | Hold **BOOT**, press and release **PWR**, release BOOT → flash again → press PWR afterwards |
| `could not open port … busy` | Something else holds the port: close the monitor (Ctrl + ]), other terminals, the emulator or log tools |
| Build errors after you edited code | Read the **first** error line: it names the file and line. Undo your last change and rebuild to confirm |
| Strange build problems after changing `sdkconfig.defaults` | Delete the `sdkconfig` file (not `.defaults`) and build again; it regenerates |
| Watch shows "clock not set" | Settings page → **Set time** (or Part 3 `settime`) |
| Screen dark but the log looks fine | Brightness: Settings page. Or press PWR |

---

## Part 2 — Using the watch

**Swipe left and right** between five pages. The dots at the bottom show where you are.

| Page | What it's for |
|---|---|
| **Watch face** | Time, status (LIVE / DEMO / LOST), K⁺ dial (green normal · amber elevated · red high), pH ring, 30 min sparkline, trend arrow. Tap the dial → Trends |
| **Trends** | K⁺ and pH charts; the button switches 5 min ↔ 30 min |
| **Calibrate** | Calibrate the electrodes (below) |
| **Diagnostics** | Raw mV, link counters, motion sensor, battery, storage, Bluetooth |
| **Settings** | Bluetooth on/off and **Mode** (Phone / Sensor / Phone + sensor), vibration, **Demo data**, K⁺ alert level, brightness, set time, clear logs |

- **Demo mode** (Settings → Demo data): fake but realistic data, labelled DEMO everywhere.
  Real data always replaces it.
- **The alert:** K⁺ above the threshold for **5 s** → red full-screen alert and vibration.
  *Dismiss* hides it until the next episode.

### Calibrating (needs real sensor data, not demo)
1. Calibrate page → pick **K+** or **pH**.
2. Electrode in the first standard → wait for **✓ STABLE**.
3. Pick that standard in the roller → **Capture standard**.
4. Repeat for 2–5 standards → **Fit & save**. Check the slope %, R² and the verdict → **Save**.

---

## Part 3 — Talking to the watch from the Mac

### The watch's command line
Open the monitor (🔥, or 🖥 *Monitor* in the bottom bar), click in the terminal, type a command,
press Enter:

| Command | What it does |
|---|---|
| `help` | List the commands |
| `status` | Time, link, latest reading, logging, memory: **the first thing to check** |
| `cal` | Stored calibrations |
| `settime 1793371622` | Set the clock (unix seconds; on the Mac `date +%s` prints the current one) |
| `ls` / `cat FILE` / `rm FILE` | List / print / delete log files |

### Tools (Terminal; close the monitor first, only one program can use the port)

Load the ESP-IDF environment once per Terminal window:

```bash
cd ~/Projects/ESP32/guder_watch && . ~/.espressif/v5.5.5/esp-idf/export.sh
```

Copy all logs to the Mac (into `watch_logs/`):

```bash
python tools/pull_logs.py /dev/cu.usbmodem3101
```

Send fake electrode data over USB (watch shows LIVE):

```bash
python tools/sensor_mcu_emulator.py /dev/cu.usbmodem3101 --usb --scenario spike
```

Stream real values from a file the instrument software is writing:

```bash
python tools/live_bridge.py /dev/cu.usbmodem3101 --usb --follow ~/Desktop/run.csv --k-col 1 --volts
```

Stop any of these with **Ctrl + C**.

---

## Part 4 — Change something yourself (worked example)

**Goal:** change the default K⁺ alert threshold from 6.0 to 5.5 mM.

1. In VS Code, open `main/Kconfig.projbuild` (press **⌘P** and type the name).
2. Find `config GW_K_ALERT_DEFAULT_CENTI_MM` and change `default 600` to `default 550`
   (hundredths of mM).
3. Delete the `sdkconfig` file so the new default is picked up. This changes the **default**
   only: a watch that already saved its settings keeps its stored value. Change that on the
   watch's Settings page.
4. 🔥 Build, flash and monitor; check it works.
5. **Save a checkpoint (commit) and upload (push):**
   - Click the **Source Control** icon (branch symbol, left sidebar).
   - Type a message: `Lower default K+ alert to 5.5 mM`.
   - Click **Commit**, then **Sync Changes**.

**Golden rules:**
- Change one thing at a time.
- Build and test before committing.
- If it breaks: Source Control → right-click the file → **Discard Changes** to undo
  uncommitted edits.

### Before committing UI or logic changes, run the safety nets

Unit tests (parser, calibration, data hub):

```bash
./test/host/run.sh
```

Screenshots of every page, no watch needed (written to `test/sim/out/`):

```bash
./test/sim/run.sh
```

---

## Part 5 — How the code works (plain language)

### The big idea
The watch is a set of **small workers running at the same time**, plus **one shared notice
board** in the middle:

```
  sensor data in ──▶ ┌──────────────────────┐ ──▶ screen (shows it)
  (UART / USB)       │  app_state           │ ──▶ logger (writes CSV)
  demo generator ──▶ │  the notice board:   │ ──▶ Bluetooth (sends to phone)
  motion + battery ─▶│  latest reading,     │ ──▶ vibration (alerts)
                     │  history, settings,  │
                     │  calibration         │
                     └──────────────────────┘
```

Workers that **produce** data pin it to the board. Workers that **use** data read the board, or
get told when something new arrives. The screen never talks to hardware directly. That
separation is what keeps the watch from freezing and lets a new sensor be added without
changing the screens.

### One reading's journey
1. The front end sends a text line: `$GKS,42,123456,152.25,-21.50,36.8,0*5C`.
2. `sensor_link.c` collects the characters into a line. `sensor_protocol.c` checks the
   checksum (`*5C`) and pulls out the numbers.
3. `app_state.c` → `app_state_push_frame()`:
   - converts mV to mM using your calibration (`calibration.c`)
   - checks the alert rule (above threshold for 5 s; clears below threshold − 0.3 mM)
   - stores it in the per-second history
   - notifies the logger, Bluetooth and vibration
4. Four times a second the screen code (`ui/ui_face.c`) reads the board and moves the dial.

### Which file does what

| File | Job | You'd edit it to… |
|---|---|---|
| `main/main.c` | Starts everything in order | add a new service |
| `main/sensor_protocol.*` | The data format | change what the front end sends |
| `main/calibration.*` | Nernst fit and conversions | change the maths |
| `main/app_state.*` | The notice board, alert rule, history | change alert behaviour |
| `main/sensor_link.c` | UART receiver + demo data | change the demo |
| `main/data_logger.c` | CSV logging | change log columns |
| `main/ble_service.c` | Bluetooth, phone side (peripheral) and mode switching | change what the phone gets |
| `main/ble_central.c` | Bluetooth, sensor side (central): scan, connect, receive frames | change how the sensor is found |
| `main/imu_service.c`, `power_service.cpp` | Motion sensor, battery  | — |
| `main/ui/*.c` | The screens (LVGL) | change how things look |
| `main/Kconfig.projbuild` | Adjustable settings (pins, defaults) | change defaults |
| `sdkconfig.defaults` | ESP-IDF settings | change system options |

### Words you'll meet

| Word | Meaning here |
|---|---|
| **Firmware** | The program that runs on the watch |
| **Flash / flashing** | Copy the built firmware onto the watch's memory |
| **Monitor** | Live text log coming back from the watch |
| **ESP-IDF** | Espressif's toolkit for building ESP32 firmware |
| **Task** | One of the workers running at the same time (FreeRTOS) |
| **Mutex / lock** | Makes workers take turns with the notice board so data isn't corrupted |
| **UART** | Simple serial wire (the RXD/TXD pads) |
| **I2C** | Two-wire bus shared by the motion sensor, clock, battery chip, touch |
| **LVGL** | The graphics library that draws the screens |
| **NVS** | Small storage that survives restarts (settings, calibration) |
| **OCP** | Open circuit potential: the electrode voltage at zero current, which is what the ISE produces |
| **Commit / push** | Save a checkpoint / upload checkpoints to GitHub |

---

## Part 6 — Learning path (about 1 hour each)

1. **Use it:** flash it, use demo mode, run `status`, pull the logs and open the CSV in Excel.
2. **Follow the data:** read `sensor_protocol.h`, then `app_state_push_frame()` in
   `app_state.c`. Run the emulator and watch `status` change.
3. **The science:** read `calibration.c` next to your own calibration (46.6 mV/decade).
   Redo the fit by hand in Excel and compare.
4. **The screen:** change one colour or text in `ui/ui_face.c`, run `./test/sim/run.sh`, look
   at the screenshot.
5. **Your driver:** reread `imu_service.c`, now knowing where its data goes
   (`housekeeping.c` → `app_state`).

---

## Part 7 — When you're stuck

1. Run `status` on the watch: it answers most "is it working?" questions.
2. Read the **first** error line, not the last.
3. Undo your last change and check it works again.
4. Ask Claude in the **ESP32-S3** project on claude.ai. It has this guide, `README.md`,
   `PROJECT_CONTEXT.md` and `ROADMAP.md`. Paste the exact error text and say what you changed.
