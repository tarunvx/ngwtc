# Changelog

All notable changes to the **Next Gen Water Tank Controller (NGWTC)** firmware.
Entries are grouped by change set. Unless noted, changes apply to both the
v5 (`esp-32-dev/`) and v6 (`v6/local-node/`) trees.

Versions are tracked per tree via `FIRMWARE_VERSION` (`config.h` for `esp-32-dev/`
and `v6/local-node/`, `tank-node.ino` for `v6/tank-node/`). MINOR is bumped on
every change; MAJOR only for a redesign.

Current: **v5.4.0** (`esp-32-dev/`) · **v6.16.0** (`v6/local-node/`) · **v6.8.0** (`v6/tank-node/`)

> **v5 (`esp-32-dev/`) has NOT been updated past 5.4.0 and now diverges.** MD1,
> the unit re-basing, CT trim and silent flags are v6-only.

---
# Version 6.16.0 — v6/local-node
## State-transition trace: every state change records why, and what the inputs were

Repeated unexplained entries into `MANUAL_ON` could not be diagnosed after the
fact. The status feed shows the *result* (`st:MANUAL_ON`) 30 seconds later, by
which time the triggering input has gone. Guessing from `ia:0`/`fl:0` was
inconclusive because those are sampled at publish time, not at the transition.

### Added

Every `enterState()` now records a `StateTrace`: the **reason**, the from/to
states, and a snapshot of **every input the decision could have depended on** —
feedback ON/OFF, current present, mV and trimmed amps, flow, level, link
liveness, all three bypass flags and `smartSense`.

31 reason codes cover each path that can change state: `BTN_MANUAL`,
`BTN_TIMER1/2`, `BTN_STOP`, `SMART_FB`, `SMART_CUR`, `SMART_FLOW`, `MD1_FB_ON`,
`MD1_FB_OFF`, `AUTO_LEVEL`, `START_OK`, `LEVEL_FULL`, `MAXRUN`, `FAULT`,
`PANIC`, `LINK_DOWN`, and so on.

Emitted three ways:

- **Serial**, immediately, one line per transition.
- **MQTT** as `TRACE:{...}` on the ack feed, paced by `FAULT_PUB_GAP_MS`.
- **`CMD:n:GET:TRACE`** replays the 16-entry RAM ring.

```
TRACE:{"n":4,"rp":0,"ts":91234,"w":"SMART_CUR","fr":"IDLE","to":"MANUAL_ON",
"md":"MANUAL","lvl":25,"fl":0,"i":61,"ia":0,
"fb1":0,"fb0":1,"cp":0,"lk":1,"bi":0,"bf":0,"bfb":1,"ss":1}
```

`w` is the answer to "what started it". `bfb` and `ss` are included precisely
because they are **not** in the status JSON, so a misconfigured bypass or
Smart-Sense flag was previously invisible.

Untagged call sites record `TR_UNKNOWN` rather than failing to compile — an
unexplained trace is still more useful than none, and names the site to tag.

---
# Version 6.15.0 — v6/local-node
## MD1 force-stop bug, operating-screen decimals, minute heartbeat

### Fixed — MD1 runs were being force-stopped, then latching

Field logs showed a real manual run (`fl:34, i:300`) reach `MANUAL_ON` and then
hit `FAULT_LATCHED` within 30 s, via repeated `NO_CURRENT` → `STOPPING` →
`NO_FEEDBACK`.

`EV_CURRENT_PRESENT(false)` while running forces `ST_STOPPING` — correct for
AUTO/MANUAL/TIMER, wrong for MD1 where the pump is under manual control. Two
consequences: a momentary current dip ended a healthy run, and entering
`ST_STOPPING` fires `actuator_pulseOff()`, which would physically stop a pump the
user wanted running. Three ERRORs inside the repeat window then latched.

**In MD1 a run now ends only on feedback-OFF, or the max-runtime backstop.**
This is the same reasoning that already excluded MD1 from the Smart-Sense paths.

### Fixed — operating screen still showed integers

6.14.0 changed the dashboard to one decimal but missed the **operating screen**,
which is the one displayed while the pump runs. Both now render `12.6L` / `10.3A`
from `sensors_currentAmpsX10()` — the same trimmed value the threshold compares
against, so screen and logic cannot disagree.

### Changed — running heartbeat is now once a minute

`BZ_RUN` was an 80 ms chirp every 2 s, which is wearing over a 20-minute fill.
Now a **long pulse (400 ms), gap, short pulse (120 ms), once every 60 s** — a
distinct "still running" signature rather than a metronome.

### Changed — `DEF_CT_THRESH_AMP_X10` 3.0 A → 1.0 A

With `ctCalAmps` trimming idle to 0.0 A, a 3.0 A threshold sat above the observed
running current (~1.4 A after trim) and reported `NO_CURRENT` on a running pump.

> **Tuning order matters:** set `CT Calib` first so an idle pump reads 0.0 A,
> then set `CT Thresh` between idle and running current. Both are in amps and
> both match the screen.

### Added

- **`Sensor Test` menu item** — enables the monitor and jumps straight to the
  live-sensor page. Previously it existed only as page 2 of `Diagnostics`,
  reachable via undocumented B2/B3 paging, so it was undiscoverable.
- **`ia` in the status JSON** — trimmed amps ×10. `i` remains raw millivolts for
  diagnostics; `ia` is the value the threshold and the screen use.

---
# Version 6.14.0 — v6/local-node
## Self-test lockout fix, CT threshold in amps, sensor page, menu rework

> Renumbered from 7.0.0. MAJOR is reserved for a hardware/architecture
> generation change; an NVS break is signalled by `SETTINGS_MAGIC`, not the
> firmware version. The rule in the instructions file was corrected to match.

### Fixed — stuck in MAINTENANCE, self-test 0x10 every boot

`selftest.cpp` compared `settings().magic` against a **hardcoded `0x5A11`** while
the layout change had moved `MAGIC` to `0x5A12`. The NVS check therefore failed
on every boot → `SELFTEST_FAIL` → `ST_MAINTENANCE`, which nothing could clear.

Three changes so this class of bug cannot recur:

- `SETTINGS_MAGIC` now lives in `settings.h` and both `settings.cpp` and
  `selftest.cpp` use it. **The literal must never be written twice.**
- **Only `ST_FAIL_FB_STUCK` now forces `ST_MAINTENANCE`.** A stuck feedback
  switch means the machine cannot tell whether the pump is running, which does
  justify withholding control. A bad NVS tag or an odd CT bias does not — those
  are reported via popup and boot continues. `ST_FAIL_*` moved to `selftest.h`.
- `sm_clearLatched()` and B3-long now clear `ST_MAINTENANCE` too. It was a dead
  end that survived reboots because the failing check was deterministic.

### Fixed — phantom pump starts (NO_CURRENT / NO_FEEDBACK / OVERRUN loop)

Field logs showed repeated `NO_CURRENT` in `MANUAL_ON` followed ~2 s later by
`NO_FEEDBACK` in `STOPPING`. Cause: the CT reads 0–2 A of noise with no clamp
connected, which occasionally crossed the 80 mV threshold. That fired
`EV_CURRENT_PRESENT(true)` → Smart-Sense started a phantom "manual run" → the
noise dropped below threshold → `NO_CURRENT` → `STOPPING` → no feedback →
`NO_FEEDBACK`. `OVERRUN` appeared when a phantom run outlived `maxRuntime`.

- Smart-Sense's current path now requires the current to be **continuously
  present for `SMART_CURRENT_CONFIRM_MS` (5 s)** and still present at the moment
  it fires. A momentary noise spike can no longer start a run.
- **MD1 is excluded from all three generic Smart-Sense paths** (current, flow,
  feedback). In MD1 the feedback switch alone drives the run, so the generic
  paths were a second, conflicting trigger — which is why a feedback-OFF press
  could put the system into operating mode.

### Changed — CT threshold is now in amps

`ctThreshMv` (mV) → **`ctThreshAmpX10`** (amps ×10, default 30 = 3.0 A), compared
against `sensors_currentAmpsX10()`. The menu value and the screen reading are now
the same number, including the `ctCalAmps` trim — previously you tuned in
millivolts while reading amps.

### Added — live sensor page (manual test)

The diagnostic screen gained a second page, toggled with **B2/B3** while diag
mode is on. RAM-only page index, so no new NVS field:

```
SENSORS   B2/B3
FB-ON:0  FB-OFF:1
FL:3.1 L  CT:12.4A
LVL: 75%  US: 77%
D:296mm  PLAUS
DHT 29.4C 61%  i412mV
```

### Changed — menu grouped and position remembered

Reordered into MODE → run behaviour → alarm → timings → levels → sensor
calibration → display → bring-up bypasses → info/danger.

- **Committing a value no longer closes the menu**, so a follow-up edit is one
  click away.
- The cursor position is remembered for **15 s** after closing, then resets to
  the top. Changing two adjacent settings previously meant scrolling the whole
  list twice.
- Stale unit labels corrected: `Dry-Run (s)`, `Max Runtime (m)`,
  `CT Thresh (A)`, `Flow Thr (L/m)`.

---
# Version 7.0.0 (renumbered to 6.14.0) — v6/local-node
## MD1 mode, user-facing units, CT trim, silent mode — **breaks NVS**

MAJOR because the NVS layout changed incompatibly. `MAGIC` is bumped
0x5A11 → 0x5A12, forcing a clean defaults load. **All stored settings are lost
on first boot** — unavoidable, because four fields changed units and the old
values would be catastrophically misread (`1800000` read as minutes is 3.4 years).

### Added — MD1 mode (`MODE_MD1 = 5`)

For installations where the pump is started only by the physical starter.
Firmware never starts the pump; it watches, alarms, and optionally stops.

```
feedback ON   -> ST_MANUAL_ON, arm the full alarm for this run
level >= 100  -> tank-full alarm; issue ONE OFF stroke (unless ignoreActuation)
feedback OFF  -> disarm, silence, return to IDLE
```

**Why the alarm latches on the feedback edge rather than the level.** The 100%
float chatters from surface turbulence while filling and for some time after the
pump stops. A level-driven alarm therefore re-triggers after every B1 silence,
and the user has to keep silencing it until the water settles. Latching on
feedback-OFF means: once the pump is confirmed off, that run's alarm is finished
and cannot be re-armed until the next manual start. `s_md1FullAlarm` is
deliberately **not** cleared when the level dips below 100.

This affects the tank-full tone only. Faults are untouched — `silentBzrOnOffFB`
cannot silence a fault.

- `ignoreActuation` — alarm only, never drive the OFF stroke (for installs with
  no relay/stroke fitted).
- `silentBzrOnOffFB` (default true) — cancel the full alarm on feedback-OFF.
- `sm_fullAlarmActive()` — lets the buzzer sound the alarm while the pump is
  still running, which `ST_FULL` alone cannot express.

### Added — `silentMode`

Global mute. Everything is suppressed **except `BZ_LATCHED`**, which needs human
intervention and so must stay audible.

### Added — `ctCalAmps` (int8, ±20 A)

The CT reads 1–2 A at rest, so the displayed value was misleading even though the
`ctThreshMv` threshold masked it for control purposes. This trims the whole
scale, applied in `sensors_currentAmpsX10()` and clamped at zero. Menu:
"CT Calib (A)". Over MQTT the value is a reinterpreted byte, so
`SET:ctCalAmps:254` means −2.

### Changed — NVS units are now what a user edits

| Was | Now | Menu |
|---|---|---|
| `dryRunMs` (u32) | `dryRunSec` (u16) | seconds, step 1 |
| `maxRuntimeMs` (u32) | `maxRuntimeMin` (u16) | minutes, step 1 |
| `timer1Ms` / `timer2Ms` (u32) | `timer1Sec` / `timer2Sec` (u16) | seconds, step 30 |
| `flowNoFlowThresh` | unchanged (lpm ×10) | now **rendered as L/min with one decimal**, step 0.2 |

Conversion to milliseconds happens at the use site via `settings_dryRunMs()`,
`settings_maxRuntimeMs()`, `settings_timer1Ms()`, `settings_timer2Ms()` — declared
in `settings.h`. **Every millis() comparison must go through these.**

The value editor gained a decimal mode (`menu_editIsDecimal()`), so the flow
threshold reads `2.0 L/min` instead of `20 x10Lpm`.

### Changed — OLED dashboard

| Row | Was | Now |
|---|---|---|
| 1 | level · state · icons | unchanged |
| 2 | `US:077%` · 4-char mode | `77%` · **3-letter mode** (`modeNameShort()`) |
| 3 | `FL:12  I:10` (whole) | `12.6L  10.3A` (**one decimal**) |
| 4–5 | temp/RH · day-time | unchanged |

---
# Version 6.13.0 / 5.4.0 — both trees
## Implausible-level fault was re-raised every tick

Bench testing with a single float switch held produced `sup=33945` in 2010 s —
about 17 suppressed duplicates per second. `sensors_tick()` runs every 50 ms and
re-raised `FC_IMPLAUSIBLE_LEVEL` on every pass for as long as the condition held,
instead of once when it became true.

The 60 s dedup kept the log from filling instantly, but over 33 minutes the
32-slot ring still ended up holding **nothing but this one fault** — so a genuine
fault in that window would have been invisible. It also pushed ~17 events/second
through the queue for no benefit.

### Fixed
- Edge-triggered: raised once on the transition into an implausible state, not
  continuously while it persists. Both trees.

---
# Version tank 6.8.0 — v6/tank-node
## TRIG settles on D8 + 1k pulldown; two "free" pins were not free

| Pin | Boot role | What else is on it | Result |
|---|---|---|---|
| GPIO3 (RX) | none | USB-serial chip's TX | drives against the ESP — sensor dead |
| GPIO0 (D3) | strap HIGH (satisfied) | DTR auto-reset transistor | held LOW with a port open — sensor dead |
| **GPIO15 (D8)** | strap LOW (violated) | **nothing** | **works, with a 1k pulldown** |

Both alternatives were electrically sound per the ESP8266 datasheet and both
failed because the *dev board* wires peripherals to them. D8 is the only pin
with nothing else attached, and its boot-strap conflict is fixed outright by one
resistor.

### Changed
- `PIN_US_TRIG` back to **GPIO15 (D8)**; **1k pulldown to GND is now a hardware
  requirement**.
- Heartbeat LED blink 2 ms → **10 ms**. At 2 ms out of 250 ms (0.8% duty) it was
  effectively invisible, which made "is the node alive?" unanswerable by eye.

---
# Version tank 6.7.0 — v6/tank-node
## TRIG to GPIO0, where the sensor's pull-up is an asset

Third and final pin for this signal. The measurement that diagnosed the original
fault — >2.5 V on D8 with reset held — also proves the JSN-SR04T pulls TRIG
*up*. GPIO0's strap must read **HIGH** at reset, so the sensor now satisfies the
requirement instead of violating it, and no pulldown resistor is needed.

| Pin | Strap needs | Sensor provides | Result |
|---|---|---|---|
| GPIO15 (D8) | LOW | pull-up | SDIO boot, hangs |
| GPIO3 (RX) | — | — | USB-serial chip drives the pin |
| **GPIO0 (D3)** | **HIGH** | **pull-up** | **works, no extra parts** |

GPIO0 also carries the FLASH button and the DTR auto-reset circuit. Neither
matters in this build: the module is pulled off the PCB to flash, and runs
sealed in a box.

### Changed
- `PIN_US_TRIG` → **GPIO0 (D3)**. D8/GPIO15 is now unconnected and the board's
  own pulldown keeps it LOW for boot.

> **One wire move at the tank node: TRIG from D8 to D3.** No resistor required.

---
# Version tank 6.6.0 — v6/tank-node
## TRIG back on D8 with a pulldown; GPIO3 is unusable on a dev board

6.5.0 moved TRIG to GPIO3 (RX) to dodge the GPIO15 boot strap. That works on a
bare ESP-12E but **not on a NodeMCU-style board**: the onboard USB-serial chip's
TX is hard-wired to GPIO3 and drives against the ESP's output. `SERIAL_TX_ONLY`
releases the pin on the ESP side only — the external chip keeps driving it. The
ultrasonic went dead.

### Changed
- `PIN_US_TRIG` back to **GPIO15 (D8)**, with an external **1k pulldown to GND**
  as a hardware requirement. It beats the sensor's pull-up
  (1/(1+10) x 5 V = 0.45 V vs a 0.825 V threshold) so the strap reads LOW at
  reset, while the ESP still drives TRIG high at ~3.3 mA.
- `SERIAL_TX_ONLY` kept: the link is one-way, so nothing is lost.

---
# Version tank 6.5.0 — v6/tank-node
## ROOT CAUSE: ultrasonic TRIG was sitting on a boot strapping pin

The tank node's long-running "needs many resets to start" fault. TRIG was on
**GPIO15 (D8)**, which the ESP8266 samples at reset to choose its boot mode and
which **must read LOW**. The JSN-SR04T pulls its TRIG input up; measured **>2.5 V
on D8 while reset was held**, against a 0.825 V threshold. GPIO15 HIGH selects
**SDIO boot**, so the chip looked for a non-existent SD card and hung silently
without ever running the sketch.

The diagnostic clue was the reset timing, which ruled out every power theory:

| Action | Result | Why |
|---|---|---|
| Brief reset tap (<1 s) | **boots** | firmware had driven TRIG LOW; pin had not drifted up yet |
| Held reset (~1 s) | fails | sensor pull-up wins, GPIO15 reads HIGH |
| Cold power-on | fails | firmware never drove it LOW, HIGH from the start |

A *longer* reset failing is impossible for a supply-ramp or POR problem, where
more settling time can only help. The capacitors were a red herring — the
correlation was with rewiring done at the same time.

### Changed
- `PIN_US_TRIG` **GPIO15 (D8) → GPIO3 (RX/D9)**, which has no boot role.
- `Serial.begin(..., SERIAL_TX_ONLY)` frees GPIO3. The link is one-way, so UART0
  never needed a receive pin.

> **Requires one wire move at the tank node:** TRIG from D8 to D9/RX.
> A 1 kΩ pulldown on D8 is a valid stopgap, but it only balances a divider
> against the sensor's pull-up; moving the pin removes the failure mode.

---
# Version 6.11.0 — v6/local-node
## Publish CRC errors in the status feed

`link_crcErrors()` was only visible on the diagnostic screen, so remote
monitoring could not tell a **bulk outage** (frames never sent/received, which
dumps a large jump into `ld`) apart from **steady corruption** (frames arriving
intact-length but failing CRC). Those have completely different causes.

### Added
- `ce` in the status JSON, next to `ld`. With `ls` it gives the same
  outage-vs-noise discrimination remotely that the OLED already had.

---
# Version 6.10.0 — v6/local-node
## Diagnostic screen, carried over from the minimal test sketch

The stripped-down test build used during the reset investigation had a single
unchanging monitor screen, which proved far easier to watch than the dashboard's
rotating content. Brought into the real firmware.

### Added
- **`Diagnostics` menu item** toggles a persistent live monitor: uptime, level,
  ultrasonic %, distance, flow, link sequence, drops, raw bytes, CRC errors,
  free heap, tank restarts and fault count. Refreshes at 1 s instead of the
  usual 2 s/5 s.
- It sits **below** popups and the menu (so it can always be switched off) but
  **above** the operating screen — while diagnosing you want one stable layout,
  not one that changes when the pump starts.
- `link_crcErrors()` — counts full frames that arrived but failed CRC. Paired
  with `link_rawBytes()` this distinguishes "bytes never arrive" (wiring) from
  "bytes arrive, bits flip" (noise), which was the key insight when diagnosing
  the 15 m link.

### Changed
- The dormant `mcuUpsPresent` settings slot was **renamed in place** to
  `diagMode` — same type and offset, so `sizeof(Settings)` and the stored NVS
  blob are unchanged and existing settings survive. (Adding a field would have
  reset NVS.) Also settable remotely via `SET:diagMode:1`.

---
# Version 6.9.0 — v6/local-node
## Fix the drop counter lying when the tank node restarts

Field data showed `"ls":2158,"ld":65816` — more drops than frames ever sent.
The tank node restarting resets its `seq` to 0, and `(uint16_t)(0 - expected)`
evaluates to ~65000, which went straight into the drop total in one hit. Every
loss percentage computed across a tank-node restart was therefore worthless.

### Fixed
- A forward sequence gap larger than `LINK_SEQ_GAP_MAX` (2000, i.e. 500 s of
  outage at 4 Hz) is now counted as a **tank-node restart**, not as lost frames.
  A natural uint16 wrap produces a gap of 0 and is unaffected.

### Added
- `link_tankRestarts()`, published in the status JSON as `tr`. Tank-node reboots
  were previously invisible except as a corrupted drop count — now they are a
  first-class signal, which matters given the ESP8266 boot-reliability issue.

---
# Version tank 6.4.0 — v6/tank-node
## Guard the ultrasonic calibration against the blind zone

`US_DIST_FULL_CM` had been set to 15 cm from a tape measure, but `US_MIN_VALID_CM`
is 20 cm (the JSN-SR04T's blind zone) and `usPingMm()` rejects anything nearer.
`usUpdate()` keeps the last good median on a miss, so filling past a 20 cm air gap
would freeze the reading at ~93% rather than reaching 100% — and
`LINK_FLAG_DIST_OK` would stay set, hiding the staleness.

Display-only: the floats remain authoritative for all control and safety logic,
so pump behaviour was never at risk.

### Changed
- `US_DIST_FULL_CM` 15 → **20 cm**, clamped to the blind zone. The top ~5 cm of
  the tank now reads as 100%. To recover that range, raise the transducer so the
  air gap at full exceeds 20 cm.

### Added
- `#error` guards for `US_DIST_FULL_CM < US_MIN_VALID_CM` and
  `US_DIST_LOW_CM <= US_DIST_FULL_CM`, so a bad calibration fails the build
  instead of silently freezing the reading.

---
# Version tank 6.3.0 — v6/tank-node
## Keep the ultrasonic ping off the wire while a frame is transmitting

The ping and the transmit were adjacent: `usUpdate()` ran immediately before
`Serial.write()`. The transducer burst pulls current through the 12–15 m ground
wire that is *also* the link's signal reference, so every frame was being clocked
out while the reference was still settling — a plausible contributor to the ~1%
byte error rate.

Worse, `write()` only fills the UART FIFO and returns; at 2400 baud the frame is
still on the wire for ~108 ms afterwards. Lowering the baud had therefore
*widened* the window in which a ping could collide with a transmission.

### Changed
- `Serial.flush()` after `write()`, so the frame is fully clocked out before
  anything else draws current.
- `usUpdate()` moved to **after** the flush and the heartbeat blink. The ping now
  lands in the idle part of the 250 ms period, ~140 ms before the next frame.
  Its reading is carried by the following frame — harmless, it is display-only
  and the tank level moves slowly.

> **Wiring note:** a Schottky in the tank node's **VCC** line is fine (reverse
> protection). A diode in the **GND** line must not be added — it would lift the
> tank node's ground above the local node's by a *load-dependent* amount, which
> is exactly the error mechanism being removed.

---
# Version 6.8.0 — v6/local-node
## Detect a floating link RX line

INT_WDT returned with the buzzer still unplugged, but only after the tank node
was physically removed — the 31-minute clean run had it connected. Disconnecting
it leaves `PIN_LINK_RX` (GPIO32) holding ~10 m of cable as an antenna against
only the internal pull-up. Spurious UART traffic would starve the FreeRTOS tick
that feeds the interrupt watchdog, which matches the observed `c1:IDLE+293`
(core 1 never scheduled, i.e. stuck in ISR context rather than in our code).

### Added
- `link_rawBytes()` — counts every byte drained from the link UART, valid frame
  or not. Reported by `GET:DIAG` as `rx=`. With no tank node attached this
  should stay at 0; if it climbs, the RX line is picking up noise and the theory
  is confirmed.

---
# Version 6.7.0 — v6/local-node
## Breadcrumb: mark IDLE on core 0 too

With the buzzer unplugged and the tank node removed, INT_WDT returned with a new
and repeatable signature — `c0:BUZZ+0 c1:IDLE+292` and `c0:BUZZ+0 c1:IDLE+295`.
`c1:IDLE` means core 1 was frozen rather than stuck in our code, but `c0:BUZZ`
was uninterpretable: core-0 tasks did not mark `IDLE`, so core 0's mark only ever
said "this task ticked most recently", not whether core 0 was running at all.
(The buzzer is physically disconnected, so it certainly was not executing.)

### Changed
- Every core-0 task now marks `BC_IDLE` before its `vTaskDelay()`, matching
  core 1. `c0:IDLE + c1:IDLE` now unambiguously means both application cores were
  parked and a **system task** (WiFi/lwIP/NVS) held interrupts off — which our
  own `BC_NVS` marks would otherwise have caught.

---
# Version 6.6.0 / tank 6.2.0 — v6 both nodes
## Link baud 9600 → 2400: ~24% of frames were being lost

30 minutes of status telemetry showed `ls` (tank sequence) climbing perfectly
monotonically — 30 → 8995 at exactly 4 Hz — so the tank node was **never**
rebooting, which is what the repeated `LINK_LOST` had implied. Instead `ld`
(drops) rose 3 → 2152 over the same window:

    loss = 2149 / 8965 = ~24% of frames
    implied byte error rate = 0.76^(1/26) => ~1%

That also predicts the dropouts: a `LINK_LOST` needs 4 consecutive misses, and
0.24^4 x 7200 frames = ~24 expected runs against ~15 observed (plus 3 suppressed).

~1% byte errors on a *wired* link is a signal-integrity problem, not power.

### Changed
- `LINK_SERIAL_BAUD` 9600 → **2400** in all three `link_proto.h` copies. Longer
  bit periods tolerate both noise and any baud/clock mismatch far better. A
  26-byte frame takes 108 ms at 2400 baud, still well inside the 250 ms
  telemetry period.
- `LINK_TIMEOUT_MS` 1000 → **2000** (4 → 8 frames). Even at the old loss rate
  this makes a spurious drop ~2000x less likely (0.24^8).

> **Both nodes must be flashed together.** Baud is not covered by
> `LINK_PROTO_VERSION`, so a mismatched pair simply stops talking.

---
# Version 6.5.0 — v6/local-node
## Network firmware update (ArduinoOTA)

The board is installed and USB is unreachable, so every test build meant
unplugging it. OTA turns that into a one-command upload over WiFi.

### Added
- `ota.{h,cpp}` — ArduinoOTA listener, `ota_init()` after `mqtt_init()` (needs
  WiFi), `ota_tick()` driven from the **MQTT task**, which is deliberately the
  one task not registered with the watchdog.
- `tasks_prepareForOta()` / `tasks_resumeAfterOta()`. An update writes ~1 MB;
  each flash write parks the other core, so the 8 s task watchdog on Control and
  Safety would fire partway through. `onStart` unregisters both from the WDT and
  suspends every task except the MQTT task running the update.
- Safety gate: `ota_tick()` will not even call `ArduinoOTA.handle()` while the
  pump is driven (`sm_isPumpRunningState()`, `ST_STARTING`/`ST_STOPPING`, or a
  pulse in flight). A reboot mid-pulse would leave the latching relay in an
  unknown state.
- `OTA_HOSTNAME` / `OTA_PASSWORD` in `SECRETS.example.h`.

### Changed
- `OTA_ENABLED` 0 → 1.

> **Partition scheme:** the Arduino `default` scheme also has two app slots, so
> OTA is possible there — but at 1.25 MB per slot the build sits at ~85%.
> `min_spiffs` (1.9 MB slots) is preferred purely for headroom. Layout cannot be
> changed over OTA, so choose it during a USB flash.

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