# Roadmap — real biosensor data on the watch

*5 Nov 2026. Times are rough estimates and depend on the lab.*

The watch side is ready: every source (UART, USB, a future ADC) feeds the single entry point
`app_state_push_frame()`, and the hub handles any sample rate, gaps, saturation, alert
debouncing and re-calibration (see PROJECT_CONTEXT.md §5). What is missing is a **measurement
of the electrodes**. This roadmap gets there in stages, each producing a usable result.

```
Phase 0  decide          → Phase 1  real data via lab instrument  → Phase 2  watch measures by itself
(questions for the lab)     (fastest real result)                    (true wearable readout)
                                                                   → Phase 3  signal quality
                                                                   → Phase 4  towards physiological use
```

---

## Phase 0 — Gather answers (about 1 week, you and the lab)

| Question | Why it matters |
|---|---|
| Which instrument measures the electrodes (make/model)? Does its software write data **during** a run? | Decides how Phase 1 connects: following a file, or a direct driver |
| State of the electrodes: working K⁺ ISE (e.g. valinomycin membrane) and a reference electrode? | Phase 1 needs at least one working ISE and a reference |
| Is a watch-mounted front end wanted? Parts, budget, anyone who does electronics? | Phase 2 scope |
| What would convince your supervisor? | Sets the success criteria for Phases 1–2 |
| Realistic alert threshold, normal range and calibration standards | Replace the placeholders |

**Done when:** the instrument and the electrodes are known, and the target demo is agreed.

---

## Phase 1 — Real electrodes through the lab instrument (1–2 weeks)

```
K+ ISE + reference ─▶ potentiostat (OCP / potentiometry, 1 Hz) ─▶ laptop ─▶ USB ─▶ watch
```

**Setup**
- Electrodes in a stirred beaker, at constant temperature if possible.
- Every solution in the same ionic background, e.g. 0.1 M NaCl or artificial interstitial or
  synovial fluid. The watch uses concentration, not activity, so the background must match.
- Condition the ISE as the lab protocol specifies before measuring.

**Steps**
1. Confirm how the instrument software saves data.
   - **Writes as it goes:** `tools/live_bridge.py --follow <file> --k-col N --volts`.
   - **Only exports at the end:** I write a direct driver for the instrument, e.g. PalmSens
     EmStat over USB with MethodSCRIPT, depending on the model.
2. Stream to the watch and check it shows `LIVE` and the raw mV.
3. **Calibrate on the watch** (Calibrate page): standards of 0.1 / 1 / 10 / 100 mM KCl. Wait
   for STABLE before each capture, then Fit & save.
4. **Validate:**
   - Check the calibration quality: slope 55–61 mV/decade, R² ≥ 0.995.
   - **Known additions:** add KCl to a beaker at a known concentration and check the watch
     reading. Target: within ±10 %, tighten later.
   - **Compare with the instrument's own calibration** on the same data. The raw mV is in
     the watch logs, so this is an offline check.
   - **Drift:** leave the electrodes in one solution for 1 h and record mV/h.
5. **Demo recording:** a video of the watch tracking K⁺ as KCl is added. This is the first
   real result.

**Firmware work (me):**
- the instrument driver, if needed
- **one-point recalibration**: re-zero the intercept with one standard between full
  calibrations, the usual ISE practice for drift
- a drift readout on the Diagnostics page

**Done when:** the watch tracks known KCl additions with a calibration meeting the criteria
above, and the run is logged.

---

## Phase 2 — The watch measures by itself (2–4 weeks, mostly waiting for parts)

```
K+ ISE ──▶ buffer ─┐
pH     ──▶ buffer ─┼─▶ ADS1115 (differential) ──I2C (header IO14/IO15)──▶ watch
reference ─▶ VREF 1.65 V ─┘
```

**Circuit (to agree with the supervisor; candidate parts):**
- **One buffer per sensing electrode.** Op-amp follower with fA–pA input bias that runs on
  3.3 V, e.g. **LMP7721** (lowest bias, needs careful layout) or **OPA320** (easier). ISEs
  have MΩ–GΩ source impedance, so the buffer is not optional.
- **Reference electrode held at mid-supply** (~1.65 V from a buffered reference or divider).
  Electrode potentials then sit inside the 0–3.3 V supply even when negative.
- **ADC:** ADS1115 (16-bit, I2C address 0x48, free on the watch's bus). Differential inputs:
  AIN0–AIN1 for K⁺ vs VREF, AIN2–AIN3 for pH vs VREF. PGA ±0.512 V gives ~16 µV per step.
  Step up to an **ADS1220** (24-bit) if noise or drift is not good enough.
- **Input protection** (series resistor plus small capacitor), **short shielded leads**, and
  guarding around the high-impedance node.
- Power from the watch's 3V3 pad. The board already has I2C pull-ups.

**Firmware work (me):**
- **`frontend_ads1x15.c` driver:** configure the ADS1115, sample both channels (~8 Hz), convert
  to mV, median-filter, set the `SATURATED` flag at full scale and `K_FAULT`/`PH_FAULT` for an
  open electrode, then push frames into `app_state`. Nothing else in the firmware changes.
- A Kconfig option to choose the source (ADC on the watch, UART, or USB) and its pins.
- ADC readings on the Diagnostics page.

**Validation:**
- Side by side with the lab instrument on the same electrodes: agreement within ~1 mV.
- Noise below ~0.1 mV RMS after filtering.
- Open-input (leakage) test, plus a touch and movement test on the leads.

**Done when:** the watch alone repeats the Phase 1 KCl-addition result.

---

## Phase 3 — Signal quality and electrochemistry (about 2 weeks, overlaps Phase 2)

| Feature | Why |
|---|---|
| **Temperature compensation** | The Nernst slope scales with absolute temperature: calibrating at 22 °C and measuring at 37 °C changes it by about 5 %. Needs a temperature sensor in the solution, e.g. a thermistor on a spare ADC channel |
| One-point recalibration and drift tracking | ISEs drift. Re-zero between full calibrations and log the drift rate |
| Filtering options (median, moving average) | Trade response time against noise |
| Motion-artefact study | The logs already pair every reading with acceleration and a motion flag. Compare voltage noise when still and when moving, then tune or justify the motion thresholds |
| Selectivity check | Measure in a physiological Na⁺ background to confirm Na⁺ interference is negligible |

---

## Phase 4 — Towards physiological use (likely beyond the internship)

- Testing in realistic matrices (artificial synovial fluid, serum); biofouling and lifetime.
- **Form factor:** where the electrodes sit relative to the watch. Implant-site K⁺ is far from
  the wrist, so the watch may end up as the reader for a separate patch or implant.
- **Bluetooth security** (pairing and encryption), low-power sleep, battery life.
- **Safety:** no current into tissue (buffer leakage, isolation), plus ethics approval before
  any human use.

---

## What is already in place (no work needed)

- Frame protocol, UART and USB inputs, the emulator, `live_bridge.py`.
- Calibration wizard, Nernst fit, slope % and R², saved calibrations, calibration log.
- Per-second history at any sample rate; correct handling of gaps, saturation and faults.
- Alert with 5 s hold and hysteresis, plus vibration and the full-screen overlay.
- CSV logging of raw mV, concentrations, motion and battery; log export over USB.
- Live data over BLE (1 update/s); diagnostics and the `status` command.
- Host tests (96 checks) and a UI renderer for checking screens without the watch.
