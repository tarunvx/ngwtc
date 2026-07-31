# Sensor Theory & Math — SWTC

> The **theory, derivations and math** behind every sensor calculation used in
> this project (the `tests/` sketches and the controller firmware). Read this to
> understand *why* each formula looks the way it does — and how to calibrate and
> debug each sensor from first principles.

---

## Table of contents

1. [Notation & conventions](#1-notation--conventions)
2. [Foundations: ADC & voltage dividers](#2-foundations-adc--voltage-dividers)
3. [Flow sensor — YF-S201](#3-flow-sensor--yf-s201)
4. [Pressure sensor — 0.5–4.5 V transducer](#4-pressure-sensor--0545-v-transducer)
5. [Ultrasonic distance — JSN-SR04T](#5-ultrasonic-distance--jsn-sr04t)
6. [Current clamp — SCT-013](#6-current-clamp--sct-013)
7. [Buzzer — passive piezo tone generation](#7-buzzer--passive-piezo-tone-generation)
8. [Signal filtering techniques](#8-signal-filtering-techniques)
9. [Constants & calibration quick reference](#9-constants--calibration-quick-reference)

---

## 1. Notation & conventions

Throughout the firmware, **floating point is avoided** in stored values and
message payloads to keep things fast and deterministic on the MCU. Instead we use
**fixed-point integer scaling**:

| Suffix | Meaning | Example |
|---|---|---|
| `_x10` | value × 10 (0.1 resolution) | `lpm_x10 = 40` → 4.0 L/min |
| `Mv` / `_mv` | millivolts | `ctThreshMv = 80` → 80 mV |
| `MPa1000` | milli-MPa (MPa × 1000) | `pressureFullMPa1000 = 100` → 0.100 MPa |
| `Cx10` | °C × 10 | `tempCx10 = 294` → 29.4 °C |

To recover the real value, divide by the scale. To display one decimal from an
`_x10` integer $v$:

$$\text{integer part} = \left\lfloor \frac{v}{10} \right\rfloor, \qquad
\text{decimal digit} = v \bmod 10$$

This is exactly what the UI/serial `printf("%u.%u", v/10, v%10)` calls do.

---

## 2. Foundations: ADC & voltage dividers

### 2.1 The ADC

An Analog-to-Digital Converter maps an input voltage to an integer code:

$$\text{code} = \left\lfloor \frac{V_\text{in}}{V_\text{ref}} \cdot (2^N - 1) \right\rfloor$$

| MCU | Resolution $N$ | Full-scale $V_\text{ref}$ | Notes |
|---|---|---|---|
| ESP32 | 12-bit (0–4095) | ~3.3 V @ 11 dB atten. | Non-linear; use `analogReadMilliVolts()` for a **factory-calibrated** mV read |
| ESP8266 (NodeMCU) | 10-bit (0–1023) | 3.3 V at the `A0` **board** pin | Board has a built-in 220k/100k divider onto the bare 1.0 V chip pin |

On ESP32 we prefer `analogReadMilliVolts()` because it applies the chip's per-unit
calibration curve, side-stepping the raw ADC's non-linearity. Converting a raw
ESP8266 count to volts at the pin:

$$V_\text{pin} = \frac{\text{raw}}{1023} \cdot 3.3$$

### 2.2 Resistor voltage divider

To read a signal larger than $V_\text{ref}$ (e.g. a 5 V logic line or a 4.5 V
sensor) we scale it down with two resistors:

```
 V_in ──[ R_top ]──┬── V_out ( → ADC )
                   │
               [ R_bot ]
                   │
                  GND
```

$$V_\text{out} = V_\text{in}\cdot \frac{R_\text{bot}}{R_\text{top}+R_\text{bot}}
\;=\; V_\text{in}\cdot r, \qquad r=\frac{R_\text{bot}}{R_\text{top}+R_\text{bot}}$$

To recover the original from the ADC reading, **multiply back up**:

$$V_\text{in} = \frac{V_\text{out}}{r}$$

Two divider choices used in this project give the **same ratio**:

$$\underbrace{\frac{20}{10+20}}_{10\text{k}/20\text{k}}
= \underbrace{\frac{30}{15+30}}_{15\text{k}/30\text{k}}
= \frac{2}{3} \approx 0.667$$

so a 4.5 V sensor maxes out at $4.5 \times 0.667 \approx 3.0\ \text{V}$, safely
inside the 3.3 V ADC range.

> **Rule of thumb:** keep the divider resistors in the 1 k–47 k range. Too high
> and the ADC's input impedance skews the reading; too low wastes current.

---

## 3. Flow sensor — YF-S201

### 3.1 How it works

The YF-S201 is a Hall-effect turbine. Water spins a magnetic rotor; each blade
passing the sensor emits one **pulse**. The pulse *frequency* is proportional to
the flow rate. The datasheet characteristic is:

$$f = 7.5 \cdot Q$$

where $f$ is the pulse frequency in Hz and $Q$ is the flow rate in **litres per
minute**.

### 3.2 Pulses per litre (the key constant $K$)

In one minute at flow $Q$: pulses $= f \cdot 60 = 7.5Q \cdot 60 = 450Q$, and the
volume delivered is $Q$ litres. Therefore:

$$K \equiv \frac{\text{pulses}}{\text{litre}} = \frac{450Q}{Q} = 450$$

This constant is the firmware setting **`flowKppl` = 450** (pulses per litre).

### 3.3 From pulses to flow rate

Counting $p$ pulses in a 1-second window gives $\text{pps} = p$ (pulses/second =
Hz). The flow rate is:

$$Q\ [\text{L/min}] = \frac{\text{pps} \cdot 60}{K}$$

Scaled to the integer `_x10` form used everywhere:

$$\boxed{\;\text{lpm\_x10} = \frac{\text{pps} \cdot 600}{K}\;}$$

**Check:** at $\text{pps}=30$ (i.e. $f=30$ Hz), $Q = 30\cdot60/450 = 4.0$ L/min,
and $\text{lpm\_x10} = 30\cdot600/450 = 40$ ✓.

### 3.4 Cumulative volume

Each pulse represents $1/K$ litres, so the running total from $P$ accumulated
pulses is:

$$\text{Litres} = \frac{P}{K}, \qquad
\boxed{\;\text{totalL\_x10} = \frac{P \cdot 10}{K}\;}$$

### 3.5 The "100× too high" bug (fixed)

The original firmware used

$$\text{lpm\_x10} = \frac{\text{pps}\cdot 60 \cdot 1000}{K}$$

which is $\times 1000$ instead of $\times 10$ — a **factor of 100 too large**. It
implicitly assumed `flowKppl` was *pulses/litre × 100* (i.e. 45000), but the
stored value was 450. The fix (§3.3) redefines `flowKppl` as plain pulses/litre,
so the existing value 450 becomes correct with **no NVS migration**.

### 3.6 Electrical note

The pulse output is **open-collector**: it can pull the line LOW but not drive it
HIGH. An external **10 kΩ pull-up** to 3.3 V is required for clean edges (the
MCU's internal ~45 kΩ pull-up is too weak and produced missed/jittery counts).
The ISR triggers on the **falling edge**.

---

## 4. Pressure sensor — 0.5–4.5 V transducer

### 4.1 Transfer function

The G1/4 stainless transducer is **ratiometric-linear**: its output voltage rises
linearly with pressure from a 0.5 V floor (0 MPa) to 4.5 V (full scale, 1 MPa):

| Pressure | $V_\text{sensor}$ |
|---|---|
| 0 MPa | 0.5 V |
| 1 MPa (full scale) | 4.5 V |

Solving the straight line for pressure:

$$P\ [\text{MPa}] = \frac{V_\text{sensor} - V_\text{min}}{V_\text{max}-V_\text{min}}\cdot P_\text{max}
= \frac{V_\text{sensor} - 0.5}{4.5 - 0.5}\cdot 1
= \boxed{\;\frac{V_\text{sensor}-0.5}{4}\;}$$

The firmware writes this as $P = (V_\text{sensor}-0.5)\times 0.25$ — identical.

### 4.2 Recovering $V_\text{sensor}$ through the divider

The 4.5 V output exceeds the 3.3 V ADC limit, so it passes through the
$r = 0.667$ divider (§2.2). End to end, on the ESP8266 tank node:

$$V_\text{pin} = \frac{\text{raw}}{1023}\cdot 3.3,
\qquad V_\text{sensor} = \frac{V_\text{pin}}{r}
\quad\Rightarrow\quad
P = \frac{V_\text{sensor}-0.5}{4}$$

The tank node ships $V_\text{sensor}$ as **native millivolts** (500–4500) over the
radio link; the ESP32 applies the MPa formula. Keeping the raw mV on the wire and
the calibration in one place means the two nodes can never disagree.

### 4.3 Useful unit conversions

$$1\ \text{MPa} = 10\ \text{bar} = 145.038\ \text{psi}$$

So a reading of $P$ MPa is $10P$ bar or $145.04P$ psi. (Water column: $1\ \text{bar}
\approx 10.2\ \text{m}$ of head, handy if you infer tank level from pressure.)

---

## 5. Ultrasonic distance — JSN-SR04T

### 5.1 Time-of-flight

The sensor emits a 40 kHz burst and raises `ECHO` HIGH for the round-trip time
$t_\text{echo}$ until the reflection returns. Distance is half the round trip:

$$d = \frac{c \cdot t_\text{echo}}{2}$$

where $c$ is the speed of sound.

### 5.2 Temperature-compensated speed of sound

Sound speed in air depends on temperature $T$ (°C):

$$c = 331.4 + 0.606\,T \quad [\text{m/s}]$$

At 25 °C, $c \approx 346.5$ m/s. Ignoring temperature causes a ~0.17 %/°C error —
about **6 cm at 3.5 m over a 20 °C swing**, which matters for tank level.

### 5.3 Practical unit form

With $t_\text{echo}$ in microseconds and $d$ in centimetres, convert $c$ from m/s
to cm/µs by dividing by 10 000:

$$d\ [\text{cm}] = t_\text{echo}\ [\mu s] \cdot \frac{c/10000}{2}$$

At ~20 °C this reduces to the familiar approximation:

$$d\ [\text{cm}] \approx \frac{t_\text{echo}\ [\mu s]}{58}$$

### 5.4 Why raw readings are "useless" — and the fixes

| Problem | Cause | Fix |
|---|---|---|
| Garbage / random values | 5 V `ECHO` into a 3.3 V pin | **1 k/2 k divider** → $5\cdot\frac{2}{3}=3.33$ V |
| Wild jumps under load | current spikes brown out the module | **100 µF cap** across sensor VCC–GND |
| Jumpy even when idle | single-ping noise | **median filter** (below) |
| Wrong near the sensor | physical dead zone | ignore < ~20–25 cm |

### 5.5 The median filter (robust to spikes)

Take $n$ (odd) pings, sort them, and use the middle value:

$$\hat d = \operatorname{median}(d_1,\dots,d_n)$$

Unlike an average, a single wild outlier cannot move the median — it just shifts
position in the sorted list. This is why median beats mean for the spiky
JSN-SR04T. A light EMA (§8.3) is then layered on top for a steady display.

### 5.6 Tank level from distance

Mounting the transducer at the top pointing down:

$$\text{water\_level} = \text{tank\_height} - d, \qquad
\text{level}\% = \frac{\text{tank\_height}-d}{\text{tank\_height}-d_\text{full}}\cdot 100$$

---

## 6. Current clamp — SCT-013

### 6.1 What the clamp produces

The SCT-013 is a **current transformer (CT)**: the mains wire threaded through it
is a 1-turn primary; the clamp has $N$ secondary turns (2000 for the -000). The
secondary current mirrors the primary, scaled down:

$$I_\text{sec} = \frac{I_\text{pri}}{N}$$

The signal is **alternating** and centred on 0 V — it swings **negative**, which a
0–3.3 V ADC cannot read directly.

### 6.2 The bias (mid-rail) circuit

Two equal resistors create a DC **mid-rail** $V_\text{off} \approx 1.65$ V; the CT
signal is injected there so the whole waveform rides between 0 and 3.3 V:

```
3.3V ─[10k]─┬─[10k]─ GND
            │
            M ≈ 1.65 V ── CT ── ADC
            │
          10µF → GND   (decouples the mid-rail)
```

Without this, the negative half-cycle clips to 0 → the classic "wrong readings."

### 6.3 Measuring AC RMS with auto-zero

The physically meaningful quantity is the **RMS** of the AC component. For samples
$x_i$ (in mV) with true mean (offset) $\mu$:

$$V_\text{rms} = \sqrt{\frac{1}{N}\sum_{i=1}^{N}\left(x_i-\mu\right)^2}$$

Expanding the square gives the **one-pass variance identity**:

$$\frac{1}{N}\sum (x_i-\mu)^2
= \frac{1}{N}\sum x_i^2 \;-\; \mu^2
= \overline{x^2} - \bar{x}^2$$

so we can accumulate $\sum x_i$ and $\sum x_i^2$ in a single loop and compute:

$$\boxed{\;V_\text{rms} = \sqrt{\overline{x^2} - \bar{x}^2}\;}$$

This **auto-zeroes** the offset: $\bar{x}$ *is* the measured mid-rail, whatever it
happens to be, so it removes any DC bias exactly. This is strictly better than the
firmware's approach of subtracting a hard-coded 1650 mV (a wrong constant offset
directly corrupts the RMS).

### 6.4 Sampling window

Mains at 50 Hz has a period $T = 1/50 = 20$ ms. To measure a whole number of
cycles (so partial-cycle bias averages out), sample over an integer multiple:

$$T_\text{window} = 200\ \text{ms} = 10\ \text{cycles} \;(50\,\text{Hz})$$

### 6.5 From RMS voltage to amps

- **Voltage-output CT** (e.g. SCT-013-030 = 30 A : 1 V, internal burden):

$$I_\text{rms} = V_\text{rms}\cdot \frac{30\ \text{A}}{1\ \text{V}}
= V_\text{rms}[\text{mV}]\times 0.030\ \tfrac{\text{A}}{\text{mV}}$$

- **Current-output CT** (SCT-013-000, no internal burden): add a **burden
  resistor** $R_b$ across the leads to turn $I_\text{sec}$ into a voltage. Size it
  so the peak stays inside the mid-rail headroom ($\lesssim 1.6$ V):

$$R_b = \frac{V_\text{peak}}{\sqrt{2}\,I_\text{sec,max}}
= \frac{1.6}{\sqrt{2}\cdot (I_\text{pri,max}/N)}$$

In all cases the cleanest path is empirical: run a **known load**, read $V_\text{rms}$,
and set $\text{A/mV} = I_\text{known}/V_\text{rms}$.

---

## 7. Buzzer — passive piezo tone generation

A **passive** piezo has no oscillator: it only makes sound when driven by an
alternating waveform. A constant DC level produces a single click, not a tone —
which is why steady `digitalWrite(HIGH)` was silent (only "kit-kit" clicks on
edges).

We generate a square wave with the ESP32 **LEDC** (PWM) peripheral:

- **Carrier frequency** $f = 2000$ Hz → period $T = 1/f = 500\ \mu s$.
- **Resolution** 8-bit → duty code $0..255$.
- **Duty** 128 = 50 % → a symmetric square wave = a clean audible tone.
- **Duty** 0 = line held low = silent.

Duty cycle vs. output:

$$\text{duty}\% = \frac{\text{code}}{2^{8}-1}\cdot 100
\quad\Rightarrow\quad 128 \approx 50\%$$

The pattern engine (chirps, beeps) simply switches the duty between 128 (tone) and
0 (silence); all the *timing* patterns are unchanged — only the pin drive is a
tone instead of a bare level.

---

## 8. Signal filtering techniques

All four sensors need noise handling. Here are the tools used and when each wins.

### 8.1 Moving average (FIR low-pass)

$$y[n] = \frac{1}{M}\sum_{i=0}^{M-1} x[n-i]$$

Smooths Gaussian/random noise; introduces a lag of about $M/2$ samples. Used for
**flow** (window of `flowAvgSamples`, default 4). Good for steady noise, but a
single large **outlier still leaks through** (÷M of it).

### 8.2 Median filter (robust / non-linear)

Sort the window and take the middle element. Completely **rejects impulse
outliers** (a lone spike never reaches the middle of the sorted list). Used for
**ultrasonic**, where the JSN-SR04T throws occasional wild values. Cost: sorting,
and it doesn't smooth small Gaussian noise as well as an average — so we combine
it with an EMA.

### 8.3 Exponential moving average (IIR low-pass)

$$y[n] = \alpha\, x[n] + (1-\alpha)\, y[n-1], \qquad 0<\alpha\le 1$$

One state variable, no buffer. Smaller $\alpha$ = smoother but slower to react.
It's a discrete first-order low-pass; the smaller $\alpha$, the longer the
effective time constant. Used as the final polish on the **ultrasonic** median.

### 8.4 RMS via the variance identity (one-pass)

Already derived in §6.3:

$$V_\text{rms} = \sqrt{\overline{x^2} - \bar{x}^2}$$

Computes the AC magnitude **and** removes the DC offset in a single accumulation
loop — ideal for the **CT clamp**.

### 8.5 Threshold with hysteresis / floor

A raw comparison "is there flow/current?" chatters near the boundary. The firmware
applies a **no-flow floor** (`flowNoFlowThresh`): values below it report exactly 0,
so sensor noise can't masquerade as a trickle. The same idea gives `ctThreshMv`
for "current present."

---

## 9. Constants & calibration quick reference

| Sensor | Key constant | Value | Meaning / formula |
|---|---|---|---|
| Flow | `flowKppl` ($K$) | 450 | pulses/litre; $\text{lpm\_x10}=\dfrac{\text{pps}\cdot600}{K}$ |
| Flow | pull-up | 10 kΩ | external, to 3.3 V (open-collector) |
| Pressure | $V_\text{min},V_\text{max}$ | 0.5 V, 4.5 V | $P=\dfrac{V_s-0.5}{4}$ MPa |
| Pressure | divider $r$ | 0.667 | 10k/20k or 15k/30k; $V_s=V_\text{pin}/r$ |
| Ultrasonic | $c$ | $331.4+0.606T$ | m/s; $d=\dfrac{c\,t_\text{echo}}{2}$ |
| Ultrasonic | ECHO divider | 1k/2k | $5\cdot\frac{2}{3}=3.33$ V |
| Ultrasonic | dead zone | ~20–25 cm | reject nearer readings |
| CT clamp | mid-rail $V_\text{off}$ | ~1.65 V | 10k/10k + 10 µF |
| CT clamp | RMS | — | $\sqrt{\overline{x^2}-\bar{x}^2}$ (auto-zero) |
| CT clamp | window | 200 ms | 10 mains cycles @ 50 Hz |
| Buzzer | LEDC | 2 kHz, 8-bit | duty 128 = tone, 0 = silent |

### Calibration recipes

- **Flow:** pass a **known volume** $V$; record total pulses $P$; set
  $K = P / V$ (pulses per litre).
- **Pressure:** at a **known pressure** (e.g. 0 with the line open), confirm
  $V_s \approx 0.5$ V; adjust the `pressure*MPa1000` endpoints if the transducer
  differs from 0.5–4.5 V.
- **Ultrasonic:** measure a **known distance** with a tape; set `AIR_TEMP_C` to
  the ambient temperature so $c$ is right.
- **CT clamp:** run a **known load** $I$; read $V_\text{rms}$; set the calibration
  $\text{A/mV} = I / V_\text{rms}$.

---

*See also: `README.md` (v5), `v6/README.md` (v6 wiring & bring-up),
`user_manual.md` (operator guide), and the sketches under `tests/` that put this
math into practice.*
