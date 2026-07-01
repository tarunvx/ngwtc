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

## 1. ESP-NOW channel — auto-discovery (Option B)

ESP-NOW peers **must share one WiFi channel**. The local node (ESP32) joins your
router as a station, so it automatically sits on **your router's 2.4 GHz
channel**. Rather than making you find/lock that channel, the tank node
**scans for your router's SSID on boot and locks ESP-NOW to whatever channel it
finds** — so it just works.

1. Set your network name in `tank-node.ino` (the **password is not needed** —
   scanning is passive):
   ```c
   #define ROUTER_SSID  "YourWiFiName"   // 2.4 GHz SSID to follow
   #define WIFI_CHANNEL 1                // fallback if SSID not found
   ```
2. That's it. On boot the tank node prints e.g.
   `found "YourWiFiName" on channel 6 … ESP-NOW channel locked to 6`. The local
   node prints its associated channel too (`[MQ] WiFi connected: … ch=6 …`) so
   you can confirm they match.

**Dual-band routers are fine.** ESP8266/ESP32 are 2.4 GHz-only, so they never see
the 5 GHz radio; the scan always returns the 2.4 GHz channel even when both bands
share one SSID. You do **not** need to split SSIDs or disable 5 GHz.

**Optional runtime resilience.** By default the tank node discovers the channel
once at boot (immune to *which* channel the router picked). If your router might
**change** its 2.4 GHz channel while the tank node stays powered, set
`CHANNEL_RESCAN_MS` (e.g. `300000` = 5 min) to periodically re-discover. Note a
re-scan pauses telemetry ~1–2 s, which the local node briefly sees as a link
blip (and, if the pump is running, would trigger a safe-stop) — hence it's off by
default. A normal site-wide power event reboots the tank node anyway, which
re-discovers on boot, so most installs never need this.

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

**Avoided on purpose:** `D0/GPIO16` (no pull-up, no interrupt — unusable for
flow), and the strapping pins `D3/GPIO0`, `D4/GPIO2`, `D8/GPIO15`.

### Pressure divider (important)

The industrial transducer outputs **0.5 V (0 MPa) … 4.5 V (1 MPa)**. NodeMCU's
`A0` board pin only tolerates **0–3.3 V**, so add an external divider:

```
   SENSOR_OUT ──[ R_top 10k ]──┬── A0
                               │
                             [ R_bot 20k ]
                               │
                              GND

   ratio = R_bot / (R_top + R_bot) = 20 / 30 = 0.667
   4.5 V × 0.667 ≈ 3.0 V  ✓ (safe, with headroom)
```

The node converts the divided reading back to the sensor's **native millivolts
(500–4500)** and sends that. The **local node applies the identical v5 MPa
formula**, so all pressure calibration stays in one place (the local node's
`pressureEmptyMPa1000` / `pressureFullMPa1000` settings — unchanged).

**Calibrate the divider:** measure the actual voltage at `A0` with a meter and
adjust `PRESS_DIVIDER_RATIO` / `PRESS_BOARD_VREF` in `tank-node.ino` until the
reported `pMv` matches the transducer's true output.

### Float wiring

Identical to v5: each float switch connects the GPIO to **GND** when submerged
(contact closed). The internal pull-up makes a dry float read HIGH. The tank
node resolves the four floats into a monotonic level and the local node treats
it exactly like v5's local floats.

### Flash settings

- Board: **NodeMCU 1.0 (ESP-12E Module)**
- Flash size: 4MB, CPU 80 MHz, Upload 115200
- Libraries: none beyond the **ESP8266 Arduino core** (`ESP8266WiFi`, `espnow`
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
| `flags`        | u8         | `PRESS_OK / FLOW_OK / ACTIVE`                         |
| `pressureMv`   | u16        | sensor's **native** mV (500–4500)                     |
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

1. Set `ROUTER_SSID` in `tank-node.ino` to your 2.4 GHz network name (channel
   is auto-discovered; `WIFI_CHANNEL` is only the fallback).
2. Flash the tank node. Open Serial @ 115200 — you should see periodic
   `[TANK] seq=… floats=0x… pMv=… pulses=…` lines.
3. Flash the local node. Open Serial @ 115200 — watch for
   `[LINK] ESP-NOW ready`, then `[LINK] UP (seq=…)` once it hears the tank node.
4. Verify on the OLED: the **antenna icon** (top-right, left of WiFi) is solid.
   Pull power on the tank node → within 1 s it becomes a **blinking X**, a
   "TANK LINK LOST" popup appears, and any running pump stops.
5. Confirm floats: lift each float and watch `level` change on the OLED / MQTT.
6. Calibrate pressure divider (section 2) and verify `pressure_mpa` in MQTT.
7. Calibrate flow with `flowKppl` (same v5 setting) against a known volume.

---

## 7. Troubleshooting

| Symptom                              | Likely cause / fix                                                |
|--------------------------------------|-------------------------------------------------------------------|
| Local node never sees `[LINK] UP`    | `ROUTER_SSID` typo/blank (tank fell back to `WIFI_CHANNEL`); compare the two nodes' printed `ch=` |
| Link flaps up/down                   | Router changed channel at runtime (set `CHANNEL_RESCAN_MS`); weak antenna; >15 ft with obstacles  |
| Pressure reads wrong                 | Divider ratio off — measure A0, tune `PRESS_DIVIDER_RATIO`/`VREF`  |
| Level stuck / implausible warnings   | Float wiring not active-LOW to GND; check pull-ups                 |
| Flow always 0                        | Flow on a non-interrupt pin; confirm D7/GPIO13; check `flowKppl`   |
| `version mismatch` (frames ignored)  | The three `link_proto.h` copies differ — re-sync them             |
| Pump won't start                     | Link down (by design) — restore tank node telemetry first         |

---

*v6 keeps the v5 brain intact and simply swaps wired sensors for a clean
wireless feed — same safety guarantees, far less noise.*
