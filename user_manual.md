# Smart Water Tank Controller — How It Works

> **Who this is for:** anyone who operates or lives with this water tank
> controller. It explains — in plain language — what the system does, what every
> button/light/sound means, and how it keeps the pump (and your tank) safe.
> No programming knowledge needed.

---

## Table of contents

1. [What this system does](#1-what-this-system-does)
2. [The 30-second overview](#2-the-30-second-overview)
3. [The parts, in plain terms](#3-the-parts-in-plain-terms)
4. [Operating modes](#4-operating-modes)
5. [The control panel — buttons](#5-the-control-panel--buttons)
6. [Reading the screen](#6-reading-the-screen)
7. [The light bar (colours)](#7-the-light-bar-colours)
8. [The beeper (sounds)](#8-the-beeper-sounds)
9. [Automatic filling explained](#9-automatic-filling-explained)
10. [Getting water when the tank is already full](#10-getting-water-when-the-tank-is-already-full)
11. [How a pump start actually happens](#11-how-a-pump-start-actually-happens)
12. [Safety protections](#12-safety-protections)
13. [Faults — what they mean and what to do](#13-faults--what-they-mean-and-what-to-do)
14. [The tank link (wireless models)](#14-the-tank-link-wireless-models)
15. [Remote control (phone / MQTT)](#15-remote-control-phone--mqtt)
16. [Settings you can adjust](#16-settings-you-can-adjust)
17. [Everyday scenarios](#17-everyday-scenarios)
18. [Quick reference card](#18-quick-reference-card)

---

## 1. What this system does

This controller automatically keeps your **overhead tank** filled from a
**borewell pump**. It watches the water level with float sensors, starts the pump
when the tank runs low, and stops it when the tank is full — while protecting the
pump from running dry, overheating, or running when something is wrong.

You can also run the pump **manually** or on a **timer** whenever you want, and
control everything from the **front panel**, the **on-screen menu**, or
**remotely from your phone**.

---

## 2. The 30-second overview

- In **AUTO** mode the system fills the tank by itself: pump **ON** when the tank
  drops to about **25%**, pump **OFF** at **100%**.
- The **screen** always shows the level, what the system is doing, water flow,
  and pump current.
- The **light bar** shows the level at a glance (and blinks red if there's a
  problem).
- The **beeper** chirps gently while the pump runs and beeps steadily when the
  tank is full.
- If anything looks wrong (no water flow, no current, pump won't switch off),
  the system **stops the pump and tells you why**.
- You can always take over **manually** — start, stop, or run for a fixed time.

---

## 3. The parts, in plain terms

| Part | What it is | What it does |
|---|---|---|
| **Controller** | The box with the screen and buttons | The "brain" — makes all decisions |
| **Borewell pump** | Submersible pump, ~250 ft down the well | Pushes water up to the tank |
| **Float sensors** | 4 floats in the tank at 25 / 50 / 75 / 100% | Tell the controller the water level |
| **Flow sensor** | A small paddle-wheel in the pipe | Confirms water is actually moving |
| **Current sensor** | A clamp around the pump wire | Confirms the motor is actually running |
| **Pressure sensor** | On the pipe | Extra reading of tank/line pressure |
| **Pump starter** | A latching relay/solenoid pair | Physically switches the pump on and off |
| **Screen** | Small OLED display | Shows status and menus |
| **Light bar** | A strip of 9 colour LEDs | Shows level and alerts |
| **Beeper** | A buzzer | Audible chirps and alarms |

> **Important about the pump starter:** it's a **latching** switch. The controller
> sends a brief **ON pulse** to start the pump and a brief **OFF pulse** to stop
> it — it does *not* hold a switch down the whole time. This is why the pump keeps
> running even if the controller restarts.

### Two hardware versions

- **Wired version (v5):** all sensors are wired directly to the controller.
- **Wireless version (v6):** a small **tank node** sits up at the tank, reads the
  floats / pressure / flow there, and sends the readings to the controller over a
  short-range radio link (ESP-NOW). Everything you see and press works the same;
  the only extra thing is a **tank-link indicator** (see §14).

---

## 4. Operating modes

| Mode | What it does |
|---|---|
| **AUTO** | Fills the tank automatically: ON at ~25%, OFF at 100%. The normal everyday mode. |
| **MANUAL** | You start and stop the pump yourself. Runs until you stop it (or a safety limit trips). |
| **TIMER** | Runs the pump for a fixed time, then stops on its own. Two presets: **Timer 1 (5 min)** and **Timer 2 (10 min)**. |
| **SLEEP** | Pauses automatic filling entirely — nothing runs on its own. Good for going away or when water supply is off. |
| **MAINTENANCE** | A safe "parked" state for servicing. The pump won't auto-start. |

You can change modes from the **menu**, the **buttons**, or **remotely**.

### Smart-Sense — noticing a pump someone started by hand

Your pump can also be switched on the **old-fashioned way** — by its own **manual
starter** (a physical switch on the pump line), completely independent of this
controller. **Smart-Sense** means the controller *notices* when that happens and
starts **monitoring and protecting** the pump automatically, even though it didn't
start it.

The moment the pump comes alive, the controller sees it through any of three
local signs:

- the **starter feedback switch** closes,
- the **motor draws current**, or
- **water starts flowing**.

When it detects any of these while sitting idle, it switches into a **monitoring
run** (shown as `MANUAL_ON`) — so the running screen, the run timer, the beeper,
and **all the safety protections** (dry-run, no-current, stuck-pump, maximum
runtime) now apply to that hand-started run, exactly as if you'd started it from
the panel. Stop the pump at its manual starter and the controller returns to idle.

> **This is why the system keeps working even if everything else is down.** The
> feedback switch and current sensor are wired *directly* to the controller, so
> they keep working **even if the wireless tank link is lost** (see §14). You can
> always get water by using the manual starter, and the controller will still
> watch over the pump. Smart-Sense can be turned off in the menu if you prefer.

---

## 5. The control panel — buttons

There are **four buttons** (B1–B4). Each does something different on a **short
press** (tap) versus a **long press** (hold ~1.5 seconds).

### Normal operation

| Button | Short press (tap) | Long press (hold) |
|---|---|---|
| **B1** | While pump running: switch between the **detailed running screen** and the **home screen**. Otherwise: **silence** the beeper. | **Start / stop MANUAL** pump run (toggles AUTO ↔ MANUAL). |
| **B2** | Start **Timer 1** (5-minute run). | **Arm / disarm "Ignore Full"** overflow run (see §10). |
| **B3** | Start **Timer 2** (10-minute run). | **HARD STOP** the pump. If already stopped and in a fault: **clear the fault**. |
| **B4** | Open the **menu**. | **Sleep** on / off. |

### Inside the menu

| Button | Action |
|---|---|
| **B2** | Move **down** the list |
| **B3** | Move **up** the list |
| **B4** | **Select** / enter |
| **B1** | **Back** / close |

### When editing a value

| Button | Action |
|---|---|
| **B3** | Increase the value |
| **B2** | Decrease the value |
| **B4** | Confirm (OK) |
| **B1** | Cancel |

---

## 6. Reading the screen

### Home screen (dashboard)

The home screen shows five rows:

| Row | Shows | Example |
|---|---|---|
| 1 | **Level %** · current **state** · link & Wi-Fi icons | `75%  IDLE  📶` |
| 2 | **Pressure** · **mode** | `0.42MPa  AUTO` |
| 3 | **Flow** (litres/min) · **current** (volts) | `12.30L  1.20V` |
| 4 | **Temperature** · **humidity** | `T:29.4C  RH:61%` |
| 5 | **Day & time** (or uptime if clock not set) | `Mon 06:45 PM` |

> When the **overflow override is armed** (§10), Row 5 is replaced by a banner:
> `** OVERFLOW ARMED 1x`.

### Running screen

Whenever the pump is active the screen **automatically switches** to a big,
easy-to-read running view: the **state**, a **run timer (MM:SS)**, the **level**,
and **flow / current**. Tap **B1** to flip back to the home screen while it runs.

The bottom line of the running screen warns you if any sensor check has been
switched off, or shows `** OVERFLOW RUN 1x **` during an overflow run.

### Pop-up messages

The screen briefly shows pop-ups for important events, e.g. `OVERFLOW ARM 1x`,
`TANK LINK LOST`, `Faults CLR`, `Rebooting..`.

---

## 7. The light bar (colours)

The strip of 9 LEDs gives you an at-a-glance readout. Under normal conditions it
shows the **water level**:

| Level | Light bar |
|---|---|
| Empty (0%) | Top LEDs **blink red** |
| Up to 25% | Top **red** |
| Up to 50% | **Orange** |
| Up to 75% | **Yellow** |
| Up to 100% | **Blue** |
| Full (100%) | **All green** |

Some system states **override** the level display:

| State | Light bar |
|---|---|
| **Serious fault (latched)** | **Fast red blink** |
| **Error (recovering)** | **Slow red blink** |
| **Sleep** | One dim **cyan** dot |
| **Maintenance** | **Amber** |
| **Full** | **All green** |

On power-up the bar runs a quick **rainbow sweep** — that's just the startup test.

---

## 8. The beeper (sounds)

| Sound | Meaning |
|---|---|
| **A few short beeps at power-up** | Startup self-check (plays along with the light-bar rainbow). |
| **Short chirp every 2 seconds** | Pump is running (a gentle "I'm on"). |
| **Steady repeating beep** | **Tank is full.** Press **B1** to silence it. |
| **Fast repeating beep** | An **error** occurred. |
| **Urgent fast beep** | A **serious (latched) fault** — needs your attention. |
| **One confirmation chirp** | You armed/disarmed the overflow override (long chirp = armed, short = off). |

**Silencing:** a short press of **B1** silences the full/error alert until the
situation changes. It does **not** disable future alerts.

---

## 9. Automatic filling explained

In **AUTO** mode:

1. When the level falls to about **25%**, the controller starts the pump.
2. It confirms the pump really started (see §11) and fills the tank.
3. At **100%** it stops the pump — the tank is full.
4. It then waits until the level drops again before refilling (a small buffer
   prevents rapid on/off cycling right at the top).

AUTO **always** stops at full — it will never deliberately overflow the tank.

> **Note:** during initial commissioning the system may be in a **monitor-only**
> setting where it watches the sensors but does not drive the pump automatically.
> Your installer switches this off to go fully live.

---

## 10. Getting water when the tank is already full

Sometimes you need water **even though the tank is full** — for example to run a
line for a while. Normally the system stops at 100%, so there's a deliberate,
**opt-in** way to run past full:

### The "Ignore Full" overflow override

This is a **one-time, safety-first override** that lets a **manual or timer** run
continue past the 100% mark.

**How to arm it — any one of these:**

- **Front panel:** **hold B2** (long press). You'll hear a confirmation chirp and
  see `OVERFLOW ARM 1x` on screen.
- **Menu:** select **"Ignore Full 1x"**.
- **Remotely:** send the `PUMP:OVERFLOW` command (see §15).

**Then start the pump** with a manual run (**hold B1**) or a timer (**tap B2** or
**B3**) — including **when the tank is already full**.

**What makes it safe:**

-  **One-shot.** It applies to **one run only**. As soon as that run stops (for
  any reason), the override is **automatically cleared**. You must arm it again
  for the next time.
-  **Auto-expires.** If you arm it but don't start a run within **60 seconds**,
  it clears itself (you'll see `OVERFLOW EXPIRED`). No stale surprises later.
-  **Never survives a restart.** If the controller reboots, the override is gone.
-  **AUTO is never affected.** Automatic filling always stops at full.
-  **All pump protections still apply** — dry-run, no-current, stuck-pump, and
  the maximum-runtime limit still stop the pump if something is genuinely wrong.
  The override only lets you **ignore the "tank full" limit**, nothing else.

**While it's active** the screen shows `** OVERFLOW ARMED 1x` (before the run) and
`** OVERFLOW RUN 1x **` (during the run) so it's never a hidden state.

To cancel before running: **hold B2 again** (or the menu item, or
`PUMP:OVERFLOW:OFF`).

---

## 11. How a pump start actually happens

When the pump is asked to start, the controller doesn't just switch it on and
hope — it **confirms each step**, in order:

1. **Send the ON pulse** to the pump starter.
2. **Confirm the starter switched on** (a feedback switch) — within ~1.5 s.
3. **Confirm the motor is drawing current** (the clamp sensor) — within ~3 s.
4. **Confirm water is flowing** (the flow sensor) — within ~15 s.

Only when **all** confirmations pass does it settle into the running state.

> **Why 15 seconds for water?** The pump sits ~250 ft down the well, so water
> takes **15–20 seconds** to climb the pipe and reach the flow sensor. The dry-run
> check waits for this on purpose — it's not a delay, it's physics.

Stopping works the same way in reverse: it sends the **OFF pulse**, then confirms
the starter released, the current stopped, and the flow stopped. If the pump
**refuses to switch off** (current or flow continues), that's treated as a serious
fault and the system latches for your attention.

If any confirmation fails, the pump is stopped and the reason is recorded (see
§13). Individual checks can be **switched off** by your installer (bypass options)
if a sensor is missing during setup — the screen warns you when a check is
bypassed.

---

## 12. Safety protections

The controller runs several independent protections at all times:

| Protection | What it guards against | What you'll see |
|---|---|---|
| **Dry-run** | Pump running with no water (empty well / air-locked) | Stops; `DRY_RUN` fault |
| **No current** | Pump supposed to run but motor isn't drawing power | Stops; `NO_CURRENT` fault |
| **No feedback** | Starter didn't actually switch | Stops; `NO_FEEDBACK` fault |
| **Overflow** | Tank filling past full | Stops at 100% (unless override armed, §10) |
| **Stuck pump** | Pump won't switch off (current/flow continues when it should be off) | Emergency stop + **latched** |
| **Maximum runtime** | Pump running too long (default 30 min) | Stops; `OVERRUN` fault |
| **Interlock** | Both starter coils energising at once | Immediate safe-off + latched |
| **Watchdog** | Software lock-up | Auto-restarts safely |

These are **defense-in-depth** — even if one check is bypassed during setup, the
others keep working.

---

## 13. Faults — what they mean and what to do

There are two levels of problem:

### Error (recoverable)

A single problem (e.g. one dry-run). The system stops the pump, shows a **slow red
blink**, beeps, and **recovers by itself after about a minute**, returning to
normal. If AUTO is on, it may try again.

### Latched fault (needs you)

A **serious** or **repeated** problem (e.g. a stuck pump, or the same error 3
times in 10 minutes). The system **stops and stays stopped**, showing a **fast red
blink** and an urgent beep. It will **not** run again until you clear it.

**To clear a latched fault:**

- **Front panel:** press **B3 long** (when the pump is already stopped), **or**
- **Menu:** **"Clear Faults"**, **or**
- **Remotely:** `SYS:RESET_FAULTS`.

> Before clearing, it's worth understanding *why* it tripped. You can review the
> recent fault history from the menu (**"Show Faults"**) or remotely
> (`GET:FAULTS`).

### Common fault names

| Name | Likely cause |
|---|---|
| `DRY_RUN` | No water reaching the tank (well dry, air lock, closed valve) |
| `NO_CURRENT` | Pump not drawing power (tripped breaker, wiring, motor) |
| `NO_FEEDBACK` | Starter didn't engage |
| `OVERRUN` | Pump ran longer than the max-runtime limit |
| `STUCK_PUMP` | Pump kept running after an OFF command |
| `IMPLAUSIBLE_LEVEL` | Float readings don't make sense (e.g. a higher float wet but a lower one dry) — often a stuck float |
| `LINK_LOST` | Lost the wireless tank link (wireless models — see §14) |

---

## 14. The tank link (wireless models)

On the **wireless version (v6)**, the level, pressure, and flow readings come from
a small **tank node** over a short-range radio link. The controller shows a small
**antenna icon** when the link is healthy.

**If the link is lost** (tank node unpowered, out of range, interference):

- The screen shows a **`TANK LINK LOST`** pop-up and a blinking link icon.
- Because the controller can no longer see the level or flow, it **fails safe**:
  - any pump run it was **driving is stopped**, and
  - **panel and app pump starts are refused** until the link is back (without a
    level reading it can't protect against overflow, and without flow it can't
    detect a dry run).
- When the link returns you'll see **`TANK LINK OK`** and normal operation
  resumes.

This is deliberate: without level and flow readings, the controller can't protect
against overflow or a dry run, so it chooses the safe option and waits.

### You can still get water — the manual starter always works

A lost link does **not** leave you without water. The pump's own **manual starter**
works no matter what — it energises the pump directly and doesn't depend on this
controller at all.

And even then you're not unprotected: because the **feedback switch and current
sensor are wired directly to the controller** (not through the tank link),
**Smart-Sense** (§4) still detects the hand-started pump and switches into a
**monitoring run**. So with the link down you still get:

- the running screen and run timer,
- the **dry-run**, **no-current**, **stuck-pump**, and **maximum-runtime**
  protections,

…just without the tank's level/flow readings (which is why the controller won't
*auto*-start on its own until the link is back). In short: **manual water always
works, and the pump is still watched over.**

> The tank node needs power at the tank end. If you ever see a persistent
> `TANK LINK LOST`, check that the tank node is powered and within range.

---

## 15. Remote control (phone / MQTT)

If your controller is connected to Wi-Fi, it publishes its status and accepts
commands over **MQTT** (used by companion apps / dashboards). It reports its full
status (level, mode, state, flow, current, faults, temperature, humidity, and
more) every **30 seconds**.

**Commands** use the form `CMD:<id>:<AREA>:<ACTION>` and get an acknowledgement
back. The common ones:

| Command | What it does |
|---|---|
| `CMD:1:PUMP:ON` | Start the pump (manual). Add `:TIMER:<minutes>` for a timed run. Wakes from Sleep. |
| `CMD:1:PUMP:OFF` | Stop the pump. |
| `CMD:1:PUMP:OVERFLOW` | Arm the one-shot "ignore full" override (§10). `:OFF` disarms it. |
| `CMD:1:MODE:AUTO` | Switch mode (`AUTO`, `MANUAL`, `TIMER`, `SLEEP`, `MAINT`). |
| `CMD:1:GET:STATUS` | Ask for a fresh status report. |
| `CMD:1:GET:FAULTS` | Report the recent fault count/history. |
| `CMD:1:SYS:RESET_FAULTS` | Clear faults. |
| `CMD:1:SYS:REBOOT` | Restart the controller. |

With the override armed, `PUMP:ON` will start **even when the tank is full**;
otherwise it politely refuses with `NOT_IDLE`.

---

## 16. Settings you can adjust

From the on-screen **menu** (B4) you can change how the system behaves. Settings
are **saved** and survive power cuts and updates. Highlights:

- **Mode:** AUTO / MANUAL / TIMER, Sleep on/off.
- **Smart-Sense:** auto-detect a pump someone started by hand and monitor it.
- **Ignore Full 1x:** arm the overflow override (§10).
- **Bypass checks:** temporarily switch off the current / flow / feedback checks
  during setup (use with care — the screen warns while bypassed).
- **Water levels:** minimum (start) and maximum (stop) percentages.
- **Pressure calibration:** 0% and 100% points.
- **Flow threshold:** how much flow counts as "water is moving".
- **Timings:** pump ON/OFF pulse length, dry-run wait, maximum runtime.
- **Timer 1 / Timer 2:** the two preset run lengths.
- **Current threshold:** how much current counts as "motor running".
- **Show uptime**, **Clear faults**, **Show faults**, **Reset defaults**,
  **Reboot**.

> The overflow override is intentionally **not** a saved setting — it always
> starts **off** after a restart, so it can never surprise you later.

---

## 17. Everyday scenarios

**"I just want the tank kept full automatically."**
Set mode to **AUTO** and leave it. It fills at ~25% and stops at 100%.

**"I want to run the pump now for a few minutes."**
Tap **B2** (5 min) or **B3** (10 min) for a timed run, or hold **B1** to run
manually until you stop it (hold **B1** again, or **B3 long** to hard-stop).

**"The tank is full but I need water in the line for a while."**
**Hold B2** to arm "Ignore Full" (chirp + `OVERFLOW ARM 1x`), then start a run
(**B1 long**, or **B2 / B3** for a timed run). It runs past full for that one run,
then clears automatically.

**"I started the pump at its own manual switch."**
That's fine — the controller notices within a moment (**Smart-Sense**), shows the
running screen, starts the run timer, and applies all the pump protections. Switch
it off at the same manual starter and the controller goes back to idle. This works
even if the tank link is down.

**"I'm going away for a week."**
Hold **B4** to put it to **Sleep**. Nothing runs on its own. Hold **B4** again to
wake it.

**"The light bar is blinking red and it's beeping."**
There's a fault. Read the screen for the reason. Fix the cause if you can, then
clear it (**B3 long** when stopped, or menu **Clear Faults**). Check **Show
Faults** for history.

**"The pump started but stopped after ~15 seconds with DRY_RUN."**
No water reached the tank in time — check the well level, valves, and pipe for an
air lock. Try again once resolved.

**"The screen says TANK LINK LOST."** *(wireless models)*
The controller can't hear the tank sensors, so it won't *auto*-start or accept a
panel/app start until the link is back — that's the safe design. **You can still
get water:** use the pump's **manual starter** directly. The controller will
notice (Smart-Sense) and monitor the run with its local protections. Meanwhile,
check that the tank node is powered and in range.

**"The controller restarted while the pump was running."**
On start-up the controller stops the pump by default. Just start it again as
usual.

---

## 18. Quick reference card

### Buttons (normal operation)

```
        SHORT (tap)                 LONG (hold ~1.5s)
 B1  Silence / switch screen    Start/stop MANUAL pump
 B2  Timer 1 (5 min)            Arm/disarm "Ignore Full"
 B3  Timer 2 (10 min)           HARD STOP  /  clear fault
 B4  Open menu                  Sleep on/off
```

### Light bar

```
 Blink red (fast) = serious fault      All green   = full
 Blink red (slow) = error/recovering   Blue        = up to 100%
 Amber            = maintenance         Yellow      = up to 75%
 Dim cyan dot     = sleep               Orange      = up to 50%
                                        Red (top)   = low / empty
```

### Sounds

```
 Chirp every 2s   = pump running
 Steady beep      = tank FULL (press B1 to silence)
 Fast beep        = error
 Urgent beep      = serious fault (needs you)
 Single chirp     = overflow override armed/disarmed
 Beeps at power-up = startup self-check
```

### Modes

```
 AUTO   fill automatically (25% -> 100%)
 MANUAL you start/stop
 TIMER  fixed-length run (5 or 10 min)
 SLEEP  nothing runs on its own
 MAINT  parked for servicing
```

---

*This document describes how the controller behaves for everyday use. For wiring,
calibration, and technical/developer details, see `README.md` and the `v6/`
documentation.*
