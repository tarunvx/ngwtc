/*
  ============================================================
   SWTC v6 — TANK NODE  (ESP8266 NodeMCU ESP-12)
  ============================================================

   Role: a "dumb", always-powered sensor relay mounted AT THE TANK.
   It reads the level floats, pressure sensor and flow meter locally
   (short wires = no CAT6 analog noise) and broadcasts the readings to
   the LOCAL NODE (ESP32 brain) over ESP-NOW ~4 times per second.

   It contains NO control logic, NO relays and NO safety state — all of
   that stays on the ESP32 local node. If this node dies or the link
   drops, the local node fails safe (stops the pump / blocks AUTO).

   ----------------------------------------------------------------
   WHY THIS NODE EXISTS
   ----------------------------------------------------------------
   In v5 the floats/pressure/flow ran ~10 m of CAT6 back to the ESP32.
   Long analog + open-collector pulse runs picked up noise, producing
   garbage flow and jittery pressure. v6 moves the ADC + pulse counting
   right next to the sensors and sends only clean digital telemetry.

   ----------------------------------------------------------------
   PIN MAP (NodeMCU ESP-12)
   ----------------------------------------------------------------
     Float 25%   -> D1 / GPIO5    INPUT_PULLUP, active-LOW (water = LOW)
     Float 50%   -> D2 / GPIO4    INPUT_PULLUP, active-LOW
     Float 75%   -> D5 / GPIO14   INPUT_PULLUP, active-LOW
     Float 100%  -> D6 / GPIO12   INPUT_PULLUP, active-LOW
     Flow (YF-S201) -> D7 / GPIO13  INPUT_PULLUP, FALLING-edge ISR
     Pressure    -> A0            via external 10k/20k divider (see below)
     Onboard LED -> GPIO2 (D4)    TX heartbeat (active-LOW)

   Avoided on purpose: D3/GPIO0, D4/GPIO2, D8/GPIO15 (boot strapping),
   and D0/GPIO16 (no pull-up, no interrupt — unusable for flow).

   ----------------------------------------------------------------
   PRESSURE SCALING  (IMPORTANT)
   ----------------------------------------------------------------
   The industrial pressure transducer outputs 0.5 V (0 MPa) .. 4.5 V (1 MPa).
   NodeMCU's A0 board pin tolerates only 0..3.3 V, so add an external divider
   to bring 4.5 V down to ~3.0 V:

        SENSOR_OUT ──[ R_top 10k ]──┬── A0
                                    │
                                  [ R_bot 20k ]
                                    │
                                   GND

     ratio = R_bot / (R_top + R_bot) = 20 / 30 = 0.667

   The node converts raw counts back into the sensor's NATIVE millivolts
   (500..4500) and ships that. The LOCAL NODE then applies the *identical*
   v5 formula  mpa = (mv/1000 - 0.5) * 0.25 , so calibration stays in one place.

   Tune PRESS_DIVIDER_RATIO / PRESS_BOARD_VREF below to match your board
   revision (some ESP-12 boards read slightly off; measure 3.30 V on A0 and
   adjust PRESS_BOARD_VREF until reported mV matches a meter).

   ----------------------------------------------------------------
   ESP-NOW CHANNEL — AUTO-DISCOVERY (Option B)
   ----------------------------------------------------------------
   ESP-NOW peers must share one WiFi channel. The ESP32 local node joins
   your router (STA), so it automatically sits on the router's 2.4 GHz
   channel. To match it WITHOUT you having to find/lock the router setting,
   this tank node SCANS for your router's SSID on boot and locks ESP-NOW to
   whatever channel that SSID is on.

   Set ROUTER_SSID below to your 2.4 GHz network name (the PASSWORD is NOT
   needed — scanning is passive). If ROUTER_SSID is left blank or the SSID
   isn't found, it falls back to the fixed WIFI_CHANNEL.

   Note: ESP8266/ESP32 are 2.4 GHz only — a dual-band router is fine; the
   MCUs simply never see the 5 GHz radio. If both bands share one SSID, the
   scan still returns the 2.4 GHz channel (the only one these chips can see).

   Optional: set CHANNEL_RESCAN_MS > 0 to periodically re-discover at runtime
   (handles a router that changes channel while the tank node stays powered).
   Default 0 = discover once at boot only (a re-scan briefly pauses telemetry,
   which the local node would see as a ~1 s link blip — hence off by default).

   Board: "NodeMCU 1.0 (ESP-12E Module)"  |  Flash 4MB  |  115200 baud
  ============================================================
*/

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "link_proto.h"

// ---- USER CONFIG -------------------------------------------
#define ROUTER_SSID         ""      // your 2.4 GHz network name (NO password needed).
                                    // Blank => skip discovery, use WIFI_CHANNEL below.
#define WIFI_CHANNEL        1       // fallback channel if ROUTER_SSID not found
#define CHANNEL_RESCAN_MS   0       // >0 = periodically re-discover router channel
                                    // (0 = boot-only; a re-scan pauses TX ~1s)
#define TELEMETRY_PERIOD_MS 250     // ~4 Hz telemetry
#define NOFLOW_PPS_FLOOR    1       // pulses/sec below this => not "active"

// Pressure divider / ADC calibration (see header notes above)
#define PRESS_DIVIDER_RATIO 0.667f  // R_bot/(R_top+R_bot)
#define PRESS_BOARD_VREF    3.3f    // full-scale volts at the A0 board pin
#define PRESS_ADC_MAX       1023.0f // ESP8266 ADC is 10-bit

// ---- Pin map -----------------------------------------------
#define PIN_FLOAT_25   5    // D1
#define PIN_FLOAT_50   4    // D2
#define PIN_FLOAT_75   14   // D5
#define PIN_FLOAT_100  12   // D6
#define PIN_FLOW       13   // D7
// A0 is the analog pin (no macro needed)

// Broadcast to everyone on-channel; the local node filters by LINK_NET_ID.
static uint8_t s_broadcast[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ---- Flow ISR ----------------------------------------------
static volatile uint32_t s_flowPulses = 0;   // window counter (reset each frame)
static volatile uint32_t s_flowTotal  = 0;   // cumulative since boot

static void IRAM_ATTR flowIsr() {
  s_flowPulses++;
  s_flowTotal++;
}

// ---- State -------------------------------------------------
static uint16_t s_seq = 0;
static uint32_t s_lastSendMs = 0;
static uint32_t s_lastWindowMs = 0;
static bool     s_lastSendOk = false;
static uint8_t  s_channel = WIFI_CHANNEL;   // resolved ESP-NOW channel (updated by discovery)
static uint32_t s_lastScanMs = 0;

// Scan for ROUTER_SSID and return the 2.4 GHz channel it's on, or 0 if the
// SSID is blank / not found. Passive scan — no password required. Blocks
// ~1.5-2 s while the radio hops all channels, so only called at boot (and,
// if enabled, on the CHANNEL_RESCAN_MS cadence).
static uint8_t discoverRouterChannel() {
  if (sizeof(ROUTER_SSID) <= 1) return 0;   // empty define => skip discovery
  Serial.printf("[TANK] scanning for SSID \"%s\"...\n", ROUTER_SSID);
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  uint8_t found = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ROUTER_SSID) {
      found = (uint8_t)WiFi.channel(i);
      Serial.printf("[TANK] found \"%s\" on channel %u (RSSI %d)\n",
        ROUTER_SSID, found, WiFi.RSSI(i));
      break;
    }
  }
  if (!found) Serial.println(F("[TANK] SSID not found — using fallback WIFI_CHANNEL"));
  WiFi.scanDelete();
  return found;
}

// Resolve the four active-LOW floats into the LINK_FLOAT_* bitmap.
// Matches v5 wiring exactly: water present grounds the input (reads LOW).
static uint8_t readFloatBits() {
  uint8_t bits = 0;
  if (digitalRead(PIN_FLOAT_25)  == LOW) bits |= LINK_FLOAT_25;
  if (digitalRead(PIN_FLOAT_50)  == LOW) bits |= LINK_FLOAT_50;
  if (digitalRead(PIN_FLOAT_75)  == LOW) bits |= LINK_FLOAT_75;
  if (digitalRead(PIN_FLOAT_100) == LOW) bits |= LINK_FLOAT_100;
  return bits;
}

// Convert raw A0 counts -> native sensor millivolts (500..4500 typical).
static uint16_t readPressureMv() {
  int raw = analogRead(A0);                       // 0..1023
  float boardV  = (raw / PRESS_ADC_MAX) * PRESS_BOARD_VREF;
  float sensorV = boardV / PRESS_DIVIDER_RATIO;   // undo external divider
  float mv = sensorV * 1000.0f;
  if (mv < 0)     mv = 0;
  if (mv > 6000)  mv = 6000;                       // clamp to sane ceiling
  return (uint16_t)(mv + 0.5f);
}

static void onSent(uint8_t* /*mac*/, uint8_t status) {
  s_lastSendOk = (status == 0);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("================================"));
  Serial.println(F("=== SWTC v6 TANK NODE (ESP8266) ==="));
  Serial.println(F("================================"));

  pinMode(PIN_FLOAT_25,  INPUT_PULLUP);
  pinMode(PIN_FLOAT_50,  INPUT_PULLUP);
  pinMode(PIN_FLOAT_75,  INPUT_PULLUP);
  pinMode(PIN_FLOAT_100, INPUT_PULLUP);
  pinMode(PIN_FLOW,      INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowIsr, FALLING);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // off (active-LOW)

  // ESP-NOW needs STA mode; we do NOT join the router on the tank node.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Auto-discover the router's 2.4 GHz channel (Option B); fall back to fixed.
  uint8_t discovered = discoverRouterChannel();
  s_channel = discovered ? discovered : (uint8_t)WIFI_CHANNEL;
  s_lastScanMs = millis();
  wifi_set_channel(s_channel);
  Serial.printf("[TANK] ESP-NOW channel locked to %u (%s)\n",
    s_channel, discovered ? "auto-discovered" : "fallback");

  Serial.print(F("[TANK] MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println(F("[TANK] ESP-NOW init FAILED — rebooting"));
    delay(1000);
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSent);
  esp_now_add_peer(s_broadcast, ESP_NOW_ROLE_SLAVE, s_channel, NULL, 0);

  s_lastWindowMs = millis();
  Serial.println(F("[TANK] ready — broadcasting telemetry"));
}

void loop() {
  uint32_t now = millis();

  // Optional runtime channel re-discovery (off by default). If the router
  // changed channel while we stayed powered, re-lock to it. The scan pauses
  // TX ~1-2 s, so the local node may log a brief link blip while this runs.
#if CHANNEL_RESCAN_MS > 0
  if (now - s_lastScanMs >= (uint32_t)CHANNEL_RESCAN_MS) {
    s_lastScanMs = now;
    uint8_t ch = discoverRouterChannel();
    if (ch && ch != s_channel) {
      Serial.printf("[TANK] channel changed %u -> %u, re-locking\n", s_channel, ch);
      s_channel = ch;
      wifi_set_channel(s_channel);
      esp_now_del_peer(s_broadcast);
      esp_now_add_peer(s_broadcast, ESP_NOW_ROLE_SLAVE, s_channel, NULL, 0);
    }
    now = millis();  // account for scan time so the send cadence stays sane
  }
#endif

  if (now - s_lastSendMs < TELEMETRY_PERIOD_MS) {
    delay(2);
    return;
  }
  uint32_t windowMs = now - s_lastWindowMs;
  s_lastWindowMs = now;
  s_lastSendMs   = now;

  // Snapshot + reset the per-window pulse counter atomically.
  noInterrupts();
  uint32_t pulses = s_flowPulses;
  s_flowPulses = 0;
  uint32_t total = s_flowTotal;
  interrupts();

  // pulses/sec for the "active" flag (avoids depending on calibration here;
  // the local node owns the LPM math + smoothing identical to v5).
  uint32_t pps = (windowMs > 0) ? (pulses * 1000UL / windowMs) : 0;

  LinkTelemetry t;
  t.netId        = LINK_NET_ID;
  t.version      = LINK_PROTO_VERSION;
  t.msgType      = LINK_MSG_TELEMETRY;
  t.floatBits    = readFloatBits();
  t.seq          = s_seq++;
  t.uptimeMs     = now;
  t.flags        = LINK_FLAG_PRESS_OK | LINK_FLAG_FLOW_OK;
  if (pps >= NOFLOW_PPS_FLOOR) t.flags |= LINK_FLAG_ACTIVE;
  t._pad         = 0;
  t.pressureMv   = readPressureMv();
  t.flowPulses   = (uint16_t)(pulses > 0xFFFF ? 0xFFFF : pulses);
  t.flowWindowMs = (uint16_t)(windowMs > 0xFFFF ? 0xFFFF : windowMs);
  t.flowTotal    = total;
  t.vbattMv      = 0;            // mains powered
  link_fillCrc(&t);

  esp_now_send(s_broadcast, (uint8_t*)&t, sizeof(t));

  // Heartbeat: brief LED blink each transmit.
  digitalWrite(LED_BUILTIN, LOW);
  delay(2);
  digitalWrite(LED_BUILTIN, HIGH);

  // Occasional serial diagnostics (every ~2 s) without flooding.
  static uint8_t dbg = 0;
  if (++dbg >= 8) {
    dbg = 0;
    Serial.printf("[TANK] ch=%u seq=%u floats=0x%02X pMv=%u pulses=%lu total=%lu win=%lums send=%s\n",
      s_channel, (unsigned)t.seq, t.floatBits, t.pressureMv,
      (unsigned long)pulses, (unsigned long)total, (unsigned long)windowMs,
      s_lastSendOk ? "ok" : "?");
  }
}
