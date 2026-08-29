# SWTC v6 — Two-Node Wireless Smart Water Tank Controller

Version 6 splits the v5 controller into **two cooperating ESP nodes** that talk
over **ESP-NOW**, eliminating the long CAT6 analog runs that plagued v5 with
flow/pressure noise.

```
   ┌──────────────────────────┐                         ┌───────────────────────────┐
   │        TANK NODE         │      ESP-NOW (2.4 GHz)  │        LOCAL NODE         │
   │   ESP8266 NodeMCU ESP-12 │  ─────────────────────► │      ESP32 WROOM-32       │
   │                          │   LinkTelemetry @ ~4 Hz │                           │
   │  • 4× level floats       │   26-byte CRC16 frame   │  • State machine / safety │
   │  • pressure transducer   │                         │  • Solenoid relays        │
   │  • YF-S201 flow meter    │                         │  • CT clamp (local)       │
   │  • mains powered, "dumb" │                         │  • DHT22 (local)          │
   │                          │                         │  • OLED UI, LEDs, buzzer  │
   └──────────────────────────┘                         │  • WiFi + MQTT, NTP       │
                                                        └───────────────────────────┘
```

The **local node is functionally identical to v5** — same state machine, menus,
safety logic, MQTT API. The only change is that **floats, pressure and flow now
arrive over the wireless link** instead of GPIO. Everything pump-side (relays,
current sensing, feedback switches, UI) stays local.

---

## Why two nodes?

In v5 the floats, pressure sensor and flow meter ran ~10 m of CAT6 back to the
ESP32. Long analog + open-collector pulse lines acted as antennas, producing
garbage flow readings and jittery pressure. v6 moves the **ADC and pulse
counting right next to the sensors** at the tank, and sends only clean digital
telemetry. Short sensor wires = no noise.

Because the submersible pump is ~250 ft down the borewell and water takes
**15–20 s** to reach the tank's flow sensor, the sub-second link latency is
irrelevant to dry-run detection.

---

## Folder layout

```
v6/
├── README.md            ← you are here
├── shared/
│   └── link_proto.h      ← MASTER copy of the on-air protocol (single source of truth)
├── tank-node/
│   ├── tank-node.ino     ← ESP8266 sketch (sensors → ESP-NOW)
│   └── link_proto.h      ← build copy (keep identical to shared/)
└── local-node/           ← full copy of esp-32-dev + wireless link layer
    ├── local-node.ino
    ├── link.h / link.cpp ← NEW: ESP-NOW receiver + liveness + getters
    ├── link_proto.h      ← build copy (keep identical to shared/)
    ├── sensors.cpp        ← floats/pressure/flow now sourced from link
    ├── state_machine.cpp  ← link-loss = fail safe; AUTO gated on link
    ├── events.h           ← EV_LINK_UP / EV_LINK_DOWN / FC_LINK_LOST
    └── … (all other v5 modules unchanged)
```

> **Keep the three `link_proto.h` copies byte-for-byte identical.** The Arduino
> IDE compiles each sketch folder in isolation, so each needs its own copy. If
> you change the protocol, bump `LINK_PROTO_VERSION` and copy the file to all
> three locations — the receiver rejects mismatched versions.

---

## 1. The wired link (replaced ESP-NOW in v6.1)

Telemetry travels over a **one-way 3.3 V UART** in the cable that already feeds
power to the tank:

```
   NodeMCU TX (D10 / GPIO1)  ------------->  ESP32 GPIO32   (PIN_LINK_RX)
   GND                       <----------->   GND            (already common)
```

Both MCUs are 3.3 V logic, so this is a **direct connection — no level shifter**.
A solid common ground is essential: UART is single-ended and has no voltage
reference without it. You already have one via the power pair.

- **Speed:** `LINK_SERIAL_BAUD` (9600) — shared in `link_proto.h` so both nodes
  agree. The 26-byte frame at 4 Hz is ~104 B/s, about **10% of capacity**.
- **Framing:** UART is a byte stream with no packet boundaries, so the three
  constant leading fields (`netId`, `version`, `msgType`) double as a **3-byte
  preamble**. The receiver hunts for that signature, collects
  `sizeof(LinkTelemetry)` bytes, then checks the CRC16. A false preamble match
  inside payload data simply fails CRC and the parser resynchronises.
- **The tank node's UART0 carries the link**, so it prints no debug text and
  blinks the onboard LED once per frame instead. Set `TANK_DEBUG 1` for text on
  UART1 (GPIO2/D4) via a USB-TTL adapter — that disables the LED.
- **The radio is switched off** on the tank node, which also removes WiFi
  interrupt jitter from the flow ISR and the ultrasonic `pulseIn` timing.

### Why the radio was dropped

ESP-NOW measured **−93 dBm with ~66% packet loss** at the installed positions,
with multi-second blackouts. Worse, attenuation varied with **tank level** (water
absorbs 2.4 GHz), so the link degraded exactly when it mattered — and because
`canStartPump()` gates on `link_alive()`, that blocks the pump. A bench RSSI
sweep showed a textbook −6 dB-per-doubling falloff, confirming the radios were
healthy and the site was simply too lossy. The wire is deterministic and reuses
the installed cable.

The tank node **broadcasts** (FF:FF:FF:FF:FF:FF) and the local node filters frames
by `LINK_NET_ID`, so there are no MAC addresses to hard-code.

---

## 2. Tank node (ESP8266 NodeMCU ESP-12)

### Pin map

| Function       | NodeMCU pin | GPIO | Mode              | Notes                          |
|----------------|-------------|------|-------------------|--------------------------------|
| Float 25 %     | D1          | 5    | `INPUT_PULLUP`    | active-LOW (water = LOW)        |
| Float 50 %     | D2          | 4    | `INPUT_PULLUP`    | active-LOW                      |
| Float 75 %     | D5          | 14   | `INPUT_PULLUP`    | active-LOW                      |
| Float 100 %    | D6          | 12   | `INPUT_PULLUP`    | active-LOW                      |
| Flow (YF-S201) | D7          | 13   | `INPUT_PULLUP`    | FALLING-edge ISR                |
| Pressure out   | A0          | ADC  | analog            | **needs divider — see below**   |
| Heartbeat LED  | D4          | 2    | onboard           | blinks on each transmit         |

**Avoided on purpose:** the strapping pins `D3/GPIO0` and `D4/GPIO2`.
`D8/GPIO15` is safe as the ultrasonic **TRIG** (it idles LOW, which is exactly
what the strap requires) and `D0/GPIO16` is safe as **ECHO** (no strapping role;
`pulseIn` polls, so its lack of interrupt support doesn't matter).

### Ultrasonic level sensor (important)

A **JSN-SR04T** replaces the old pressure transducer. It is mounted on **top** of
the tank pointing down, so the measured distance is the **air gap** above the
water:

```
   small distance  =  FULL tank
   large distance  =  EMPTY tank
```

Two constants in `tank-node.ino` map that gap onto **0–100 %**:

| Constant | Default | Meaning |
|---|---|---|
| `US_DIST_FULL_CM` | `20` | air gap at **100 %** (water near the sensor) |
| `US_DIST_LOW_CM`  | `100` | air gap at **0 %** (water far away) |

Measure both on the real tank and set them. Keep `US_DIST_FULL_CM` at or above
the sensor's **~20–25 cm dead zone** or the "full" end reads unreliably.

**ECHO is a 5 V output** — it must be divided down before `GPIO16`:

```
   ECHO ──[ R1 1k ]──┬── D0 / GPIO16
                     │
                 [ R2 2k ]
                     │
                    GND        5 V × 2/3 = 3.33 V  ✓
```

Also fit a **100 µF capacitor across the sensor's VCC/GND** — its ping current
spikes otherwise brown out the transducer and produce garbage readings. One ping
is taken per telemetry frame and passed through a **rolling median**
(`US_SAMPLES`) to reject the module's occasional wild outliers. The node ships
both the raw distance (mm) and the mapped percent.

> `A0` is now **unused** and free for future expansion.

### Float wiring

Identical to v5: each float switch connects the GPIO to **GND** when submerged
(contact closed). The internal pull-up makes a dry float read HIGH. The tank
node resolves the four floats into a monotonic level and the local node treats
it exactly like v5's local floats.

### Flash settings

- Board: **NodeMCU 1.0 (ESP-12E Module)**
- Flash size: 4MB, CPU 80 MHz, Upload 115200
- Libraries: none beyond the **ESP8266 Arduino core** (`ESP8266WiFi` — used only
  to switch the radio off)
  are bundled)

---

## 3. Local node (ESP32 WROOM-32)

Wiring is **unchanged from v5** — see `local-node/pins.h`. The only pins that are
now *unused* are the former float / flow / pressure inputs (they're free for
future use). The CT clamp, solenoid relays, feedback micro-switches, DHT22,
OLED, NeoPixels, buttons and buzzer are all still local.

### What changed vs v5

| Module              | Change                                                                 |
|---------------------|------------------------------------------------------------------------|
| `link.cpp/.h`       | **NEW** — ESP-NOW receiver, CRC validation, liveness, snapshot getters |
| `sensors.cpp`       | floats/pressure/flow read from `link_*()`; flow diffs cumulative pulses |
| `state_machine.cpp` | `EV_LINK_DOWN` → safe-stop + `FC_LINK_LOST`; AUTO/MANUAL/TIMER gated on `link_alive()` |
| `events.h`          | added `EV_LINK_UP`, `EV_LINK_DOWN`, `FC_LINK_LOST`                      |
| `ui.cpp`            | antenna indicator in header (blinking X when down) + link popups        |
| `mqtt.cpp`          | status JSON gains `link`, `link_age_ms`, `link_seq`, `link_drops`       |
| `tasks.cpp`         | control task calls `link_tick()` each loop                              |
| `local-node.ino`    | `link_init()` after `mqtt_init()` (WiFi must be in STA mode first)       |

### Flash settings

- Board: **ESP32 Dev Module** (WROOM-32)
- Same libraries as v5 (Adafruit SSD1306/GFX, DHT, Adafruit MQTT). ESP-NOW
  (`esp_now.h`) ships with the ESP32 core.
- Copy `SECRETS.example.h` → `SECRETS.h` and fill in WiFi/MQTT creds (same as v5).

---

## 4. Link-loss safety behavior

The link is considered **DOWN** if no valid frame arrives for **1 s** (4 missed
telemetry frames). On the transition:

| Condition at link loss | Action                                                            |
|------------------------|-------------------------------------------------------------------|
| Pump running/starting  | `FC_LINK_LOST` (ERROR) → **STOPPING** (pump safely shut off)       |
| Idle                   | `FC_LINK_LOST` (WARN) logged to fault history                     |
| Any                    | OLED shows blinking-X link icon + "TANK LINK LOST" popup           |
| While down             | **all pump starts refused** (AUTO, MANUAL, TIMER) — no level/flow  |

Rationale: without the tank node we have **no level** (overflow protection gone)
and **no flow** (dry-run detection gone), so running the pump blind is unsafe.
When telemetry resumes, `EV_LINK_UP` fires, the flow baseline re-primes (no bogus
spike), and normal operation continues.

---

## 5. On-air protocol (`link_proto.h`)

A single 26-byte packed frame, broadcast ~every 250 ms:

| Field          | Type       | Meaning                                              |
|----------------|------------|------------------------------------------------------|
| `netId`        | u8         | `LINK_NET_ID` (0x57) — filters foreign packets        |
| `version`      | u8         | `LINK_PROTO_VERSION` — receiver rejects mismatch      |
| `msgType`      | u8         | `LINK_MSG_TELEMETRY`                                  |
| `floatBits`    | u8         | `LINK_FLOAT_25/50/75/100` bitmap (active-LOW resolved)|
| `seq`          | u16        | rolls over; local node counts gaps                    |
| `uptimeMs`     | u32        | tank node millis() (diagnostics)                      |
| `flags`        | u8         | `DIST_OK / FLOW_OK / ACTIVE`                          |
| `usLevelPct`   | u8         | ultrasonic level **0–100 %** (mapped on tank node)    |
| `distanceMm`   | u16        | ultrasonic **air gap** in mm (0 = no echo)            |
| `flowPulses`   | u16        | pulses this window (diagnostics)                      |
| `flowWindowMs` | u16        | window length (diagnostics)                           |
| `flowTotal`    | u32        | **cumulative** pulses since boot (local node diffs)   |
| `vbattMv`      | u16        | reserved (0 — mains powered)                          |
| `crc16`        | u16        | CRC16-CCITT over all preceding bytes                  |

**Integrity:** the link runs **unencrypted** because ESP-NOW encryption is *not*
cross-compatible between ESP8266 and ESP32. A **CRC16-CCITT** rejects corrupted
frames; `netId` + `version` + `msgType` filtering rejects foreign/garbage
packets. Using the **cumulative** `flowTotal` (rather than per-packet counts)
means a dropped frame never loses pulses.

---

## 6. Bring-up checklist

1. Wire the tank node's **TX (GPIO1)** to the local node's **GPIO32**, with a
   common ground. No `ROUTER_SSID`/channel setup is needed any more.
2. Flash the tank node. Open Serial @ 115200 — you should see periodic
   `[TANK] seq=… floats=0x… dist=…mm lvl=…% pulses=…` lines.
3. Flash the local node. Open Serial @ 115200 — watch for
   `[LINK] ESP-NOW ready`, then `[LINK] UP (seq=…)` once it hears the tank node.
4. Verify on the OLED: the **antenna icon** (top-right, left of WiFi) is solid.
   Pull power on the tank node → within 1 s it becomes a **blinking X**, a
   "TANK LINK LOST" popup appears, and any running pump stops.
5. Confirm floats: lift each float and watch `level` change on the OLED / MQTT.
6. Calibrate the ultrasonic level: measure the air gap at full and empty, then
   set `US_DIST_FULL_CM` / `US_DIST_LOW_CM` and verify `us_level` in MQTT.
7. Calibrate flow with `flowKppl` (same v5 setting) against a known volume.

---

## 7. Troubleshooting

| Symptom                              | Likely cause / fix                                                |
|--------------------------------------|-------------------------------------------------------------------|
| Local node never sees `[LINK] UP`    | TX not landing on GPIO32, no common GND, or baud mismatch (`LINK_SERIAL_BAUD`) |
| Link flaps up/down                   | Router changed channel at runtime (set `CHANNEL_RESCAN_MS`); weak antenna; >15 ft with obstacles  |
| Ultrasonic reads 0 / jumps around    | Missing 1k/2k ECHO divider or 100 µF cap; target closer than the ~20 cm dead zone |
| Level stuck / implausible warnings   | Float wiring not active-LOW to GND; check pull-ups                 |
| Flow always 0                        | Flow on a non-interrupt pin; confirm D7/GPIO13; check `flowKppl`   |
| `version mismatch` (frames ignored)  | The three `link_proto.h` copies differ — re-sync them             |
| Pump won't start                     | Link down (by design) — restore tank node telemetry first         |

---

*v6 keeps the v5 brain intact and simply swaps wired sensors for a clean
wireless feed — same safety guarantees, far less noise.*
