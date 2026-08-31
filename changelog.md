# Changelog

All notable changes to the **Next Gen Water Tank Controller (NGWTC)** firmware.
Entries are grouped by change set. Unless noted, changes apply to both the
v5 (`esp-32-dev/`) and v6 (`v6/local-node/`) trees.

Versions are tracked per tree via `FIRMWARE_VERSION` (`config.h` for `esp-32-dev/`
and `v6/local-node/`, `tank-node.ino` for `v6/tank-node/`). MINOR is bumped on
every change; MAJOR only for a redesign.

Current: **v5.3.0** (`esp-32-dev/`) · **v6.4.0** (`v6/local-node/`) · **v6.1.0** (`v6/tank-node/`)

---
# Version 6.4.0 / 5.3.0 — both trees
## Root cause of the INT_WDT resets: the DHT22 read

Breadcrumb `c0:BUZZ+0 c1:SENS+302` — core 1 spent 302 ms inside `sensors_tick()`
against a 300 ms INT_WDT. The mark was `SENS`, not `IDLE`, so it was genuinely
executing rather than parked, which ruled out the earlier flash-write theory.
Everything else in `sensors_tick()` is `digitalRead`s, arithmetic and the CT
busy-wait (interrupts enabled) — the DHT22 bit-bang is the only code in that
function that disables interrupts (Adafruit's `InterruptLock`). A marginal
sensor makes `expectPulse()` grind through its per-pulse timeouts with
interrupts off, and the 3-attempt retry loop multiplied the exposure.

### Changed
- **One DHT transaction per cycle** instead of 3 retries. The library caches
  readings for 2 s, so the retries returned the same stale failure anyway while
  still costing interrupts-off time — they added risk and no benefit.
- **Failure backoff:** after `DHT_FAIL_LIMIT` (5) consecutive failures the poll
  interval drops from 2.5 s to 30 s, cutting exposure ~12x. Logs on recovery.
  DHT is display-only, so slower polling costs nothing operationally.

### Fixed
- `dhtFails` was incremented unconditionally *after* the success `break`, so it
  never actually sat at 0 after a good read — the "persistent read failures"
  message could fire on a healthy sensor, and any backoff keyed on it would
  have been wrong.

---
# Version 6.3.0 — v6/local-node
## Breadcrumb: separate "parked" from "stuck", and see NVS writes

The first breadcrumb read `c0:LED+0 c1:ACT+294` — core 1 frozen 294 ms against a
300 ms INT_WDT. But `actuator_tick()` early-returns when no pulse is active, so
the mark was really covering the following `vTaskDelay()`: core 1 was *parked*,
not stuck. That is the signature of a flash write on the other core, which calls
`spi_flash_disable_interrupts_caches_and_other_cpu()`.

### Added
- `BC_NVS` — marked inside `settings_save()` and the fault-log `persist()`, the
  two places that write flash, so a stall there names itself.
- `BC_IDLE` — marked before `vTaskDelay()` in the core-1 tasks (Sensor, Control,
  Safety). Core-0 tasks deliberately keep marking their own subsystem so the
  *initiating* core stays identifiable.

### Fixed
- `bc_report()` printed a core that had never marked as a huge age (`NONE+4182`,
  which read like a 4.2 s stall but only meant "core 0's tasks did not exist
  yet"). It now prints `-`.

---
# Version 6.2.0 — v6/local-node
## Crash breadcrumb for INT_WDT diagnosis

### Added
- `breadcrumb.{h,cpp}` — per-core "what was running" marker in `RTC_NOINIT_ATTR`
  memory, which survives a watchdog/panic reset but not a power cycle.
  `bc_mark()` is called from `tasks.cpp` before every subsystem tick, so the
  instrumentation lives in one file instead of being scattered.
- `bc_captureBoot()` runs first in `setup()` and formats the previous boot as
  `c0:<phase>+<age> c1:<phase>+<age>`. Because `INT_WDT` freezes both cores at
  once, **the core with the larger age is the suspect**.
- Reported on serial as `[BC] last breadcrumb:` and over MQTT in `GET:DIAG`.

### Changed
- `GET:DIAG` now leads with `fw=<FIRMWARE_VERSION>`; `mqtt_publishAck()` line
  buffer 96 → 160 B to fit it.
- Boot banner and sketch header now read `FIRMWARE_VERSION` instead of a
  hardcoded string (applied to both trees).

---
# Version 5.2.2
## LED level bar — count & fill direction

### Changed
- Reduced `NEO_COUNT` to **9** in `pins.h` (removed the dead LED).
- Reversed the fill direction — the bar now fills from the **top** (high index)
  downward, matching the inverted physical mounting:

  | Level | LEDs lit | Colour |
  |---|---|---|
  | 0%   | top 2 (idx 8, 7) | red (blinking) |
  | 25%  | top 3 | orange |
  | 50%  | top 5 | yellow |
  | 75%  | top 7 | blue |
  | 100% | all 9 | green |

---

## Actuator — active-LOW relay logic

### Changed
- The relay modules are **active-LOW** (opto-isolated). Previously both drive
  pins sat LOW at idle, keeping the coils energised and drawing current
  continuously. The logic was reversed so coils are **off at idle**:

  | State | `PIN_RELAY_ON` | `PIN_RELAY_OFF` | Effect |
  |---|---|---|---|
  | Idle | HIGH | HIGH | Both coils off — zero current draw |
  | Pulse ON | LOW | HIGH | ON relay energised, OFF safe |
  | Pulse OFF | HIGH | LOW | OFF relay energised, ON safe |
  | Interlock fault | LOW | LOW | Both energised → panic, force both HIGH |

- Relays now draw current only during the brief pulse window (default **1 s**),
  then return to idle (HIGH = coil off).

---

## LED — startup rainbow animation

### Added
- `led_init()` now plays a rainbow chase on boot:
  - **Chase (~1 s):** each LED lights in turn with a 2-LED fading trail across
    the rainbow (red → orange → yellow → green → cyan → blue → violet).
  - **Hold (0.4 s):** full rainbow across all LEDs.
  - **Fade-out (~0.2 s):** all LEDs dim smoothly to black.
- Total ~**1.6 s**, after which the normal level display takes over.

---

## LED level display & DHT22 reliability

### Fixed
- **LED level display:** the old code lit LEDs from the top down and showed only
  a single LED at 0% (looked broken). Rewritten to fill from the **bottom up**:

  | Level | LEDs | Colour |
  |---|---|---|
  | 0%     | 2 | red (blinking) — empty warning |
  | <25%   | 2 | red (solid) |
  | 25–49% | 3 | orange |
  | 50–74% | 5 | yellow |
  | 75–99% | 7 | blue |
  | 100%   | all | green |

- **DHT22 temp/humidity:** timing-sensitive reads failed under FreeRTOS
  preemption; a single failed read left values at the sentinel (−9999), showing
  `T:--.-C`. Added a **3-attempt retry** per cycle, raised the read interval to
  **2.5 s**, and added failure logging to Serial.

---

## Flow gating, feedback bypass, sleep bug, level topic, serial

### Changed
- **Flow sensor — reverted pump gating.** On a shared water line the flow sensor
  must report real readings even when this pump is off, so the pump-state gate
  was removed. Averaging (4 samples) + a higher threshold (2.0 L/min) filters
  cable noise while still showing real flow from either system.

### Added
- **`bypassFeedback` setting** — menu item *"Bypass Feedback"* and
  `CMD:x:SET:bypassFeedback:1`. When on, STARTING skips the feedback-switch wait
  and STOPPING doesn't fault on missing feedback. Combined with I+F bypass,
  STARTING passes immediately → `MANUAL_ON`.
- **Dedicated level MQTT topic** — water level (0–100) also publishes to
  `.../swtc-slash-level` for cross-tank coordination.

### Fixed
- **`PUMP:ON` → SLEEP bug.** `sleepMode` persisted in NVS, and the IDLE tick
  (`sm_tick`) pushed the system back to SLEEP every 20 ms — so any error after
  `PUMP:ON` shoved it into SLEEP. The B1-long / B2 / B3 handlers now clear
  `sleepMode` before entering STARTING, and MQTT `PUMP:ON` **wakes** from SLEEP
  instead of rejecting the command.
- **LED symptom:** the LED "bug" was really the state machine being stuck in
  ERROR/SLEEP, whose LED overrides (red blink / dim blue) hid the level display.
  With the SM staying in IDLE correctly, level colours show again.

### Serial / debug
- 500 ms delay for USB-CDC enumeration (was 200 ms — too short for some USB
  chipsets), explicit `Serial.flush()` after the boot banner, and a
  `=== Baud: 115200 ===` reminder.

> **Note:** the `Settings` struct grew (added `bypassFeedback`), so NVS
> auto-resets to defaults on first boot — `sleepMode` false, all bypass flags
> false. Re-enable bypasses via menu/MQTT after flashing.

---

## Operating screen, sensor bypass, flow smoothing, remote actuation

### Added
- **Operating screen** (`ui.cpp`, `ui.h`, `state_machine.cpp`): while the pump is
  active (STARTING/RUNNING/STOPPING) the OLED auto-switches to a big-font view —
  state + runtime (MM:SS), level %, flow (L/min) + current (mV), and bypass
  warnings. B1 short-press toggles between the operating screen and the home
  dashboard; refresh is **2 s** during a run (vs 5 s idle).
- **`bypassCurrentSense` / `bypassFlowSense`** (`settings.*`, `menu.cpp`,
  `state_machine.cpp`): menu items *"Bypass I-Sense"* / *"Bypass Flow"*
  (default off). When bypassed, STARTING skips current/flow confirmation,
  running states don't fault on loss, and STOPPING won't panic on residual
  readings; the operating screen shows `!! I+F BYPASSED !!`. Settable via
  `CMD:x:SET:bypassCurrentSense:1`.
- **Flow smoothing & threshold** (`sensors.cpp`, `settings.*`, `menu.cpp`):
  configurable moving average (1–8 samples, default 4) and a no-flow threshold
  (`flowNoFlowThresh`, default 5 = 0.5 L/min) below which flow reports 0. Output
  shown as a clean `X.Y L/min`. Both settable via menu (*"Flow Threshold"*) and
  MQTT.

### Changed
- **Remote actuation** (`mqtt.cpp`):
  - `CMD:x:PUMP:ON` → switch to MANUAL + STARTING (respects bypass flags).
  - `CMD:x:PUMP:ON:TIMER:10` → 10-minute timer run.
  - `CMD:x:PUMP:OFF` → hard stop (ACKs `NOT_RUNNING` if idle).
  - Added an `ALREADY_RUNNING` guard against duplicate starts.
  - Status JSON now includes `bypass_i` and `bypass_f`.