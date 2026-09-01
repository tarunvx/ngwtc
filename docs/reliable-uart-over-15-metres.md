# Getting Reliable UART Over 15 Metres of Unshielded Wire — Without RS-485

**An engineering postmortem on a 3.3 V single-ended serial link between two ESP nodes, how it lost a quarter of its packets, and the five changes that made it dependable.**

---

## Summary

A wired UART link ran 12–15 m from a controller to a sensor node on three
unshielded conductors: VCC, GND, and a single signal wire. It worked, in the
sense that data arrived — but it was silently discarding **24% of frames**, and
the application layer above it declared the link dead roughly **15 times per
half hour**.

The instinctive fixes — a bigger power supply, a differential transceiver — were
both wrong or unnecessary. The actual causes were a shared ground carrying pulsed
load current, a transducer firing mid-transmission, and a floating input acting
as an antenna.

| Metric | Before | After |
|---|---|---|
| Frame loss | 24% | 15% |
| Implied byte error rate | ~1.05% | ~0.63% |
| Spurious link-loss events | ~184 / day | ~0.1 / day |
| Phantom flow readings at rest | 68–127 L/min | 0.0 L/min |
| Transceiver hardware added | — | none |

The headline is the third row. **Frame loss went down by a third; user-visible
failures went down by three orders of magnitude.** Those are different numbers
for a reason, and the gap between them is the most transferable lesson here.

---

## 1. The system

Two microcontrollers, ~15 m apart:

- **Local node** (ESP32) — the brain. State machine, relays, UI, WiFi/MQTT.
- **Tank node** (ESP8266) — at the top of a water tank. Reads four float
  switches, an ultrasonic distance sensor, and a flow meter.

The tank node transmits a **26-byte binary telemetry frame every 250 ms** (4 Hz),
one-way, over plain UART. The frame carries a fixed 3-byte preamble, sensor
values, a sequence number, and a CRC16-CCITT.

```
26 bytes: netId | version | msgType | floatBits | seq | uptimeMs |
          flags | usLevelPct | distanceMm | flowPulses | flowWindowMs |
          flowTotal | vbattMv | crc16
```

Three conductors run between the nodes:

```
LM2596 buck ──┬── VCC ────────── 15 m ──────────► tank node Vin
              ├── GND ────────── 15 m ──────────► tank node GND
              └────────── signal ── 15 m ────────► local node RX (GPIO32)
```

Unshielded, untwisted, running through a building near pump wiring. This link
replaced a 2.4 GHz radio scheme that measured −93 dBm with ~66% loss at the
installed positions, with attenuation that varied with **tank water level** —
i.e. it degraded exactly when it mattered. A wire was the right call. The wire
just needed to actually work.

---

## 2. Instrument before you theorise

The single most valuable decision was refusing to guess. Four counters were added
to the receiver, published over MQTT:

| Counter | Meaning | Answers |
|---|---|---|
| `ls` | last sequence number received | is the sender alive and counting? |
| `fr` | frames that passed CRC | how many actually made it? |
| `ld` | frames missing, from sequence gaps | how many were lost? |
| `ce` | frames that arrived complete but failed CRC | corrupted vs. absent? |
| `rx` | **raw bytes** drained from the UART, valid or not | is anything on the wire at all? |

`rx` deserves special mention. It counts bytes *before* any parsing, which makes
it the only counter that can distinguish these three states:

- `rx = 0` → dead wire, or the sender isn't transmitting
- `rx > 0`, `fr = 0` → traffic exists but nothing decodes (baud mismatch, or
  catastrophic corruption)
- `rx` and `fr` both climbing → link works; the ratio is the quality metric

Every conclusion below rests on these counters. Several plausible-sounding
theories were killed by them within minutes.

**A sanity check worth building in:** `fr + ld` should equal the change in `ls`.
If that identity doesn't hold, your counters are lying and any percentage you
compute from them is fiction. (It didn't hold at first. See §8.)

---

## 3. First finding: the sender was innocent

The obvious hypothesis for intermittent dropouts is that the remote node keeps
crashing — brownouts at the end of a long thin supply wire are a classic.

`ls` disproved it in one line. Over 30 minutes it climbed **30 → 8995**,
perfectly monotonically, at almost exactly 120 per 30-second window:

```
120 frames / 30 s = 4 Hz            <- exactly the telemetry rate
```

A node that reboots resets its sequence counter. This one never did — not once
in half an hour. **The tank node's power was fine**, and a 470–1000 µF capacitor
"to stop it browning out" would have been the right part fitted for entirely the
wrong reason.

Over that same window, `ld` rose 3 → 2152:

```
loss = 2149 / 8965 = 24%
```

So: the sender transmits reliably, and a quarter of what it sends never arrives
intact. That is a transmission-path problem, full stop.

### From frames to bits

A 26-byte frame survives only if all 26 bytes survive. Assuming independent byte
errors:

```
P(byte ok) = P(frame ok) ^ (1/26)
           = 0.76 ^ (1/26)
           = 0.9895        ->  byte error rate 1.05%
```

**A byte error rate of ~1.05%.** For a wired link that is enormous — orders of
magnitude worse than a UART should ever be. Something was actively corrupting
bits, and it was worth understanding what before reaching for hardware.

---

## 4. Root cause: the signal ground is also the power return

Here is the physics, and it is the heart of this article.

UART is **single-ended**: the receiver decides whether a bit is 0 or 1 by
comparing the signal wire against **its own local ground**. It has no other
reference. The ESP32's input thresholds are ratios of its supply:

```
V_IL(max) = 0.25 x VDD = 0.825 V     <- anything below this reads as 0
V_IH(min) = 0.75 x VDD = 2.475 V     <- anything above this reads as 1
```

Now look at the wiring again. **The tank node's entire supply current returns
through the same GND conductor the UART is referenced to.** That wire is not
ideal. For 15 m of 24 AWG (~0.084 Ω/m):

```
R_gnd = 15 m x 0.084 ohm/m = 1.26 ohm
```

At a modest 150 mA of load:

```
V_offset = I x R = 0.150 A x 1.26 ohm = 0.19 V
```

The remote node's ground now sits ~190 mV above the receiver's ground. A logic
low transmitted as 0 V arrives as +190 mV. Still below 825 mV, so a *static*
offset alone is survivable.

**The problem is that the offset is not static.** It is I x R, and the current I
varies — sharply — with what the remote node is doing. Every ultrasonic ping,
every burst of activity modulates the load current, which modulates the ground
offset, which moves the receiver's decision threshold **while a frame is in
flight**. Bits get sampled against a reference that is shifting underneath them.

That is a mechanism that produces exactly what was observed: bytes that mostly
arrive, with sporadic single-bit corruption, at a rate that no amount of
retransmission logic would fix.

There was corroborating evidence sitting in plain sight the whole time: the buck
converter feeding the run had been set to **5.5 V rather than 5.0 V**, precisely
to compensate for voltage drop over that cable. That drop was known. What wasn't
appreciated is that *half of it lands on the signal reference.*

> **Generalisable rule:** if a single-ended signal shares its return path with
> pulsed load current, the signal's noise margin is set by the load's transient
> behaviour, not by the signal driver.

---

## 5. The fixes

Five changes, in rough order of impact per unit of effort.

### 5.1 Keep burst current local: bulk capacitance

The ultrasonic transducer fires a short, high-current burst. Drawing that
through 15 m of wire is what shakes the ground reference. Drawing it from a
capacitor 20 mm away does not.

Fitted at the remote node:

- **1000 µF electrolytic** across VCC/GND — the bulk reservoir
- **100 µF** directly across the ultrasonic module's own supply pins
- **0.1 µF ceramic** in parallel with each

The ceramics are not optional decoration. An electrolytic has meaningful ESR and
ESL; above a few hundred kHz its impedance rises and it stops behaving like a
capacitor. The fast edges — precisely the ones coupling into your ground — live
in exactly that band. Electrolytic for bulk energy, ceramic for high frequency.
Both, always.

The reframing matters more than the part: **this capacitor is a signal-integrity
component, not a power-supply component.** It was originally specified to prevent
a brownout that §3 proved never happened.

### 5.2 A Schottky in VCC — and emphatically **not** in GND

A series **Schottky** (1N5817, ~0.3 V) in the VCC line gives reverse-polarity
protection and stops the local reservoir discharging backwards up the line when
the shared rail dips — which matters here, because the same buck converter also
drives relay coils.

Use Schottky rather than silicon: a 0.7 V drop on a supply already compensating
for cable loss is expensive. Budget it:

```
 5.5 V   supply
-0.4 V   cable drop
-0.3 V   Schottky
-------
 4.8 V   at the load
```

With 5 V sensors downstream, that is tight. Measure at the load, under load.

**Do not put a diode in the ground line.** It is tempting for symmetry, and it is
actively harmful. A diode's forward drop varies with current, so it would lift
the remote ground above the receiver's by a *load-dependent* amount — which is
precisely the failure mechanism of §4, deliberately installed. The one wire that
must be a clean, low-impedance, shared reference is ground.

### 5.3 Slow the baud rate down

Counter-intuitive, but the strongest single lever available in software.

```
bit time @ 9600 baud = 1 / 9600 = 104 us
bit time @ 2400 baud = 1 / 2400 = 417 us     <- 4x more time per bit
```

A UART samples near the centre of each bit. A longer bit period means:

- More tolerance to ground-reference excursions, which are transient — a
  disturbance occupying a fixed number of microseconds corrupts a smaller
  fraction of a longer bit.
- More tolerance to baud/clock mismatch, which accumulates across the 10 bits of
  a character.
- Less high-frequency content in the signal, so less capacitive coupling into and
  out of adjacent conductors.

You are trading bandwidth for margin. The budget made this free: a 26-byte frame
every 250 ms needs only

```
minimum baud = (26 bytes x 10 bits) / 0.250 s = 1040 baud
chosen baud  = 2400                            -> 2.3x headroom
```

so 2400 baud still has better than 2× headroom. **If you are not bandwidth-bound,
you are leaving noise immunity on the table by running fast.**

### 5.4 Don't transmit and draw current at the same time

The remote node was doing this, per cycle:

```c
usUpdate();                                  // ultrasonic burst — current spike
...
Serial.write((const uint8_t*)&t, sizeof(t)); // frame goes out
```

The transducer fired immediately before every transmission, so every frame was
clocked out while the ground reference was still recovering. Simple time-division
fixes it — the two events are moved as far apart as the 250 ms period allows.

**But there is a trap, and it is worth the whole section.** `Serial.write()` does
not transmit. It copies into the UART FIFO and returns, typically in
microseconds. The bytes are still being shifted onto the wire long afterwards:

```
time on the wire = (26 bytes x 10 bits) / 2400 baud = 108 ms
```

Reordering alone would have accomplished nothing, because the "later" ping would
still land inside the 108 ms transmission window. The fix requires an explicit
flush:

```c
Serial.write((const uint8_t*)&t, sizeof(t));
// write() only fills the FIFO; at 2400 baud the frame is still on the wire for
// ~108 ms after it returns. Wait for it before drawing any burst current.
Serial.flush();

// Ping only once the frame is fully clocked out. The transducer burst pulls
// current through the shared ground wire that is also the link's signal
// reference. The reading is carried by the next frame.
usUpdate();
```

Note the interaction with §5.3: **lowering the baud rate widened this collision
window 4×**, from 27 ms to 108 ms. Two independently sensible changes that make
each other worse if you apply only one of them. The measurement window moved from
"probably overlapping" to "certainly overlapping".

Cost: the ultrasonic reading is carried by the *next* frame, adding 250 ms of
latency to a value used only for display. For a tank that fills over 20 minutes,
irrelevant.

### 5.5 Terminate your inputs: floating pins are antennas

The flow meter is an open-collector output. Its input pin relied on the MCU's
internal pull-up — roughly 30–100 kΩ.

A high-impedance node with a length of wire attached is an antenna. With the
sensor disconnected, the interrupt handler counted **~511 phantom edges per
second**, which the application faithfully rendered as **68–127 L/min of water
flowing through a pump that was switched off**.

The arithmetic confirms the mechanism exactly. With a 450 pulse/litre sensor:

```
pps = 68.1 L/min x 450 pulses/L / 60 s = 511 Hz
```

An external **10 kΩ** pull-up lowers the node impedance by roughly an order of
magnitude and the phantom pulses vanish. Post-fix, the reading with the pump off
is a rock-steady **0.0 L/min**.

The same reasoning applies to the receiver's **UART RX pin**. Most UART drivers
enable an internal pull-up, but on a long cable that is still a high-impedance
node. An external 4.7–10 kΩ pull-up is cheap insurance — and it matters most in
the failure case, when the cable is severed or the remote node is dead. A
controller that destabilises when its sensor cable is cut is worse than one that
cleanly reports "link down".

**Rule of thumb:** any input that is not actively driven, on a wire longer than a
few centimetres, wants an external pull-up in the kΩ range. Internal pull-ups are
for buttons on a PCB.

---

## 6. Design for loss, don't just fight it

Frame loss improved from 24% to 15%. That is a real gain but hardly a triumph,
and on its own it would not have made the system usable.

The change that actually eliminated user-visible failures was **admitting the
link is lossy and sizing the timeout accordingly.**

The receiver declared the link dead after 1000 ms without a valid frame — four
frames at 4 Hz. Assuming independent losses, the probability of four consecutive
failures at a loss rate of 0.24:

```
0.24 ^ 4 = 0.0033                       (1 chance in 300)
0.0033 x 7200 frames per 30 min = 24    spurious dropouts predicted
``` The
logs showed ~15 recorded plus 3 suppressed duplicates. Same order of magnitude —
the model was right, and the "dropouts" were mostly a statistical artefact of an
aggressive timeout rather than a link that had genuinely died.

Widening the timeout to 2000 ms (8 frames), at the improved loss rate of 0.152:

```
0.152 ^ 8 = 0.00000028                        (1 chance in 3.5 million)
0.00000028 x 345600 frames per day = 0.1      dropouts per day
```

**From ~184 per day to about one every ten days.** Same wire, same loss rate,
three orders of magnitude fewer failures.

This only works because the protocol was designed to tolerate loss:

**Send cumulative counters, not deltas.** The flow meter reports a monotonically
increasing lifetime pulse count, and the receiver differences it over its own
window:

```c
uint32_t total  = link_flowTotalPulses();
uint32_t pulses = (total >= prevTotal) ? (total - prevTotal) : 0;  // restart => re-baseline
prevTotal = total;
```

A dropped frame loses *nothing* — the next good frame still carries the full
total. Had the frame carried "pulses since last frame", every one of those 15%
lost frames would have permanently under-counted the water delivered. **With a
15% loss rate, a delta-encoded counter would have read 15% low, forever.**

The same principle covers the rest of the payload: every frame is a complete,
self-contained snapshot of absolute state. There is no field whose meaning
depends on having received the previous frame. That property is what buys you the
freedom to simply ignore a lost packet.

---

## 7. Resolution: where the remaining loss lives

After the fixes, a representative window:

```json
{"ls":1871, "fr":1587, "ld":285, "ce":259, "rx":48701, "tr":0}
```

First, the identity holds — the counters can be trusted:

```
fr + ld = 1587 + 285 = 1872        vs  ls = 1871      <- accounts for everything
```

Now the interesting part:

```
rx / 26 = 48701 / 26 = 1873         vs  ls = 1871
```

**Essentially every frame's worth of bytes is arriving.** Combined with
`ce = 259`, the picture is that ~14% of frames arrive *complete but corrupted*,
and only ~27 were lost to byte-level desynchronisation.

This is a genuinely useful distinction that `ld` alone cannot make:

| Symptom | Diagnosis |
|---|---|
| `rx` far below `ls × 26` | bytes never arrive — wiring, level, or sender fault |
| `rx ≈ ls × 26`, high `ce` | bytes arrive, bits flip — **noise / reference problem** |

Back-solving the byte error rate:

```
frame success = fr / ls = 1587 / 1871 = 0.848
P(byte ok)    = 0.848 ^ (1/26)        = 0.9937
byte error    = 0.63%                            (was 1.05%)
```

Down from 1.05%, so the interventions roughly halved the bit error rate. What
remains is inherent to a single-ended signal sharing a return path with load
current over 15 m — the topology itself.

**The honest conclusion: this is not a clean link. It is a lossy link that the
protocol renders reliable.** For telemetry that is entirely legitimate. For
anything requiring guaranteed delivery, add a differential transceiver.

---

## 8. Measurement pitfalls that cost us time

Three bugs in the *instrumentation* — each of which briefly produced a confident,
wrong conclusion.

### Unsigned modular arithmetic in the gap counter

```c
uint16_t expected = (uint16_t)(prevSeq + 1);
if (t->seq != expected) drops += (uint16_t)(t->seq - expected);   // BUG
```

When the sender restarts, its sequence resets to 0, and `(uint16_t)(0 - 500)`
evaluates to **65036**. One reboot injected ~65000 phantom "drops" in a single
step, producing `"ls":2158, "ld":65816` — more losses than frames ever sent.

The fix turns the bug into a feature. A gap larger than any plausible outage
*is* a restart, so count it as one:

```c
uint16_t gap = (uint16_t)(t->seq - expected);
// A restart resets seq to 0; unsigned maths turns that into a ~65000 "gap".
// A natural uint16 wrap yields gap 0, so it is unaffected.
if (gap > LINK_SEQ_GAP_MAX) tankRestarts++;
else                       drops += gap;
```

Note the subtlety in the comment: natural counter wrap at 65535 → 0 produces
`expected = 0` and `gap = 0`, so modular arithmetic handles it correctly. Only a
genuine reset-to-zero misbehaves. Remote reboots went from *corrupting* the loss
metric to being their own first-class counter.

### The reset reason after a USB flash

`esp_reset_reason()` reported `OTHER_WDT` on a node that had not crashed. Cause:
the flashing tool resets the chip out of download mode using the RTC watchdog.
**The first reset reason after any USB upload is the programmer's, not a fault.**
Only trust it from the second boot onwards.

### Quantisation masquerading as noise

The flow rate jittered between 2.8 and 3.8 L/min and looked like electrical
noise. It was arithmetic. At ~3 L/min with a 450 pulse/litre sensor:

```
pps            = 3 L/min x 450 pulses/L / 60 s = 22.5 Hz
per 250 ms frame = 22.5 x 0.250                = 5.6 pulses
```

With fewer than six counts per sample, **±1 pulse is ±18%**. No amount of
averaging fixes resolution that isn't there — averaging quantised samples still
averages their error. Counting over a 3-second window instead gives ~67 pulses:

```
over 250 ms:  5.6 pulses  ->  +/-1 pulse = +/-18%
over 3 s:      67 pulses  ->  +/-1 pulse = +/-1.5%      <- 12x better
```

A twelvefold improvement, at the cost of 3 seconds of latency on a value that
changes over minutes. **When a measurement is noisy, check whether you have
enough counts before you reach for a filter.**

---

## 9. What we deliberately did not do

**RS-485.** A differential pair with a MAX485 at each end would very likely have
taken loss to zero, because it is immune to exactly the ground-offset mechanism
of §4 — the receiver compares the two signal lines against *each other*, not
against a shared ground.

It was not adopted because the link became reliable enough without it, and every
change above cost pennies and no new failure modes. **Reach for RS-485 when: the
run exceeds ~15 m, you cannot separate signal ground from power return, the
environment is electrically hostile, or you need guaranteed delivery rather than
best-effort telemetry.** Three of those four applied here; the fourth did not,
and that is what made the cheap path viable.

**A dedicated signal-ground conductor** is the intermediate option and probably
the best value of all: a fourth wire, star-connected at the receiver, carrying
*no* power current. It removes the §4 coupling mechanism outright rather than
damping it, at the cost of one conductor. Take it if you are pulling new cable.

---

## 10. Takeaways

1. **Instrument first.** Five counters — `rx`, `fr`, `ld`, `ce`, `ls` — converted
   a vague "it drops out sometimes" into a measured 24% loss with a known
   mechanism. Nearly every hypothesis that felt obvious was killed by data within
   minutes of the data existing.
2. **A monotonic sequence number is the cheapest diagnostic in embedded systems.**
   One field proved the sender never rebooted and demolished the leading theory.
3. **Single-ended signalling is only as good as its ground.** If the return path
   carries pulsed load current, your noise margin is set by the load, not the
   driver.
4. **Bulk capacitance at the remote end is a signal-integrity fix**, not just a
   brownout fix. Pair every electrolytic with a ceramic.
5. **Never put a diode in a shared ground.** Load-dependent reference offset is
   the disease, not the cure.
6. **If you are not bandwidth-bound, slow down.** Baud rate buys noise immunity
   at no cost when your duty cycle is 1%.
7. **`write()` is not `transmit()`.** Know when your bytes are actually on the
   wire before you sequence anything against them.
8. **Floating inputs are antennas.** ~511 phantom interrupts per second became
   127 L/min of imaginary water.
9. **Design for loss.** Cumulative counters and a correctly sized timeout turned
   a 15%-loss link into one that fails visibly about once every ten days.
10. **Distrust your instruments too.** An unsigned subtraction, a bootloader
    reset, and a quantisation artefact each produced a confident wrong answer
    before the underlying physics was ever in question.

The most transferable idea is #9. Considerable effort went into reducing the
error rate, and it improved by roughly a third. The change that actually made the
system dependable was accepting the residual error and building a protocol that
does not care about it.
