#include "sensors.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include "events.h"
#include "event_queue.h"
#include "time_utils.h"
#include "ui.h"
#include "link.h"   // v6: floats/pressure/flow arrive over ESP-NOW

#if HAS_DHT22
  #include <DHT.h>
  static DHT dht(PIN_DHT22, DHT22);
#endif

// ============== Pressure sensor (v6: sourced from tank node) ====
static float s_pressureMPa = 0.0f;

// ============== Flow (v6: cumulative pulses from tank node) =====
static uint32_t s_flowLastAggMs   = 0;
static uint32_t s_flowTotalPulses = 0;   // mirrors tank-node cumulative count
static uint32_t s_flowPrevTotal   = 0;   // previous cumulative for 1s delta
static bool     s_flowPrimed      = false;
static uint16_t s_lpmX10          = 0;


// Moving average buffer for flow smoothing
#define FLOW_AVG_MAX 8
static uint16_t s_flowHistory[FLOW_AVG_MAX] = {0};
static uint8_t  s_flowHistIdx = 0;
static uint16_t s_flowSmoothed = 0;  // smoothed lpm_x10

// ============== float resolver =============
static uint8_t s_lastLevel = 0;
static bool    s_levelPlausible = true;

// ============== current sense (CT) ==========
static uint16_t s_lastCurrentMv = 0;
static bool     s_currentPresent = false;

// ============== feedback ====================
static bool s_fbOn = false, s_fbOff = false;

// ============== DHT cache ===================
static int16_t  s_tempCx10 = -9999;   // sentinel = no reading yet
static uint16_t s_rhX10 = 0;

void sensors_init() {
  // v6: floats, pressure and flow are NOT wired to this node anymore —
  // they arrive over ESP-NOW from the tank node (see link.cpp). Only the
  // pump-side sensors remain local: feedback micro-switches, CT clamp, DHT22.
  pinMode(PIN_FB_ON,  INPUT_PULLUP);
  pinMode(PIN_FB_OFF, INPUT_PULLUP);

#if HAS_CT_CLAMP
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CT_ADC, ADC_11db);
#endif

#if HAS_DHT22
  dht.begin();
  Serial.println(F("[SEN] DHT22 init"));
#endif

  Serial.println(F("[SEN] v6: floats/pressure/flow sourced from tank node via link"));
}

static uint8_t readFloats() {
  // v6: floats live on the tank node; the link layer resolves the
  // active-LOW pattern into a level % and a monotonic plausibility flag
  // (identical semantics to v5's local float wiring).
  s_levelPlausible = link_levelPlausible();
  return link_levelPct();
}

static uint16_t sampleCtRmsMv() {
#if HAS_CT_CLAMP
  // Sample for ~20 ms (one mains cycle at 50 Hz) — 100 samples @ ~5 kHz
  const int N = 200;
  uint32_t sumSq = 0;
  uint16_t off = settings().ctOffsetMv;
  for (int i = 0; i < N; i++) {
    uint32_t mv = analogReadMilliVolts(PIN_CT_ADC);
    int32_t  d  = (int32_t)mv - (int32_t)off;
    sumSq += (uint32_t)(d * d);
    delayMicroseconds(100);
  }
  uint32_t mean = sumSq / N;
  return (uint16_t)sqrt((double)mean);
#else
  return 0;
#endif
}

void sensors_tick() {
  // ---- pressure sensor (every 1s, sourced from tank node) ----
  static uint32_t lastPressure = 0;
  if (elapsed(lastPressure, 1000)) {
    lastPressure = millis();
    // Tank node sends the sensor's NATIVE millivolts (e.g. 500..4500).
    // Apply the SAME mapping as v5 so calibration semantics are unchanged:
    //   0.5V = 0 MPa, 4.5V = 1 MPa  =>  mpa = (V - 0.5) * (1/4)
    float v = link_pressureMv() / 1000.0f;
    float mpa = (v - 0.5f) * (1.0f / 4.0f);
    if (mpa < 0) mpa = 0;
    if (mpa > 1.2f) mpa = 1.2f;
    s_pressureMPa = mpa;
  }

  // ---- DHT22 (every 2s — sensor minimum interval) ----
#if HAS_DHT22
  static uint32_t lastDht = 0;
  static uint8_t  dhtFails = 0;
  if (elapsed(lastDht, 2500)) {  // 2.5s to give sensor extra recovery time
    lastDht = millis();
    // DHT22 is timing-sensitive; retry up to 3 times
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
      float h = dht.readHumidity();
      float t = dht.readTemperature();
      if (!isnan(h) && !isnan(t)) {
        s_tempCx10 = (int16_t)(t * 10.0f);
        s_rhX10    = (uint16_t)(h * 10.0f);
        dhtFails = 0;
        break;
      }
      delayMicroseconds(100);  // brief gap before retry
    }
    if (dhtFails < 255) dhtFails++;
    // Log persistent failures (first 5 only to avoid spam)
    if (dhtFails == 5) {
      Serial.println(F("[SEN] DHT22: persistent read failures"));
    }
  }
#endif

  // ---- level ----
  // Floats are discrete (0/25/50/75/100), so emit on any change. (v5 had a
  // dead hysteresis guard here that never actually gated anything — the
  // `|| lvl != s_lastLevel` made the outer test always true. Simplified.)
  uint8_t lvl = readFloats();
  if (lvl != s_lastLevel) {
    s_lastLevel = lvl;
    Event e{}; e.type = EV_LEVEL_CHANGED; e.p.i32 = lvl;
    sendEvent(e);
    ui_requestUpdate(); // Ensure UI updates immediately on level change
  }

  // Only flag implausible level while the link is alive — a dead link would
  // otherwise spam stale/zero float patterns. Link-loss is handled separately.
  if (link_alive() && !s_levelPlausible) {
    Event e{}; e.type = EV_FAULT;
    e.p.fault = { FC_IMPLAUSIBLE_LEVEL, SEV_WARN };
    sendEvent(e);
  }

  // ---- feedback ----
  bool fbOn  = (digitalRead(PIN_FB_ON)  == LOW);
  bool fbOff = (digitalRead(PIN_FB_OFF) == LOW);
  if (fbOn != s_fbOn) {
    s_fbOn = fbOn;
    Event e{}; e.type = EV_FB_ON; e.p.boolean = fbOn;
    sendEvent(e);
  }
  if (fbOff != s_fbOff) {
    s_fbOff = fbOff;
    Event e{}; e.type = EV_FB_OFF; e.p.boolean = fbOff;
    sendEvent(e);
  }

  // ---- current (CT) ----
  static uint32_t lastCt = 0;
  if (elapsed(lastCt, 250)) {
    lastCt = millis();
    s_lastCurrentMv = sampleCtRmsMv();
    bool present = s_lastCurrentMv >= settings().ctThreshMv;
    if (present != s_currentPresent) {
      s_currentPresent = present;
      Event e{}; e.type = EV_CURRENT_PRESENT; e.p.boolean = present;
      sendEvent(e);
    }
  }

  // ---- flow aggregation (v6: difference tank-node cumulative pulses) ----
  // We diff the tank node's monotonic flowTotal once per second. Using the
  // cumulative counter (instead of a per-packet count) means a dropped
  // telemetry frame doesn't lose pulses — the next good frame still carries
  // the full total. All downstream math/smoothing matches v5 exactly.
  if (elapsed(s_flowLastAggMs, 1000)) {
    s_flowLastAggMs = millis();

    uint32_t total = link_flowTotalPulses();
    uint32_t p = 0;

    if (!link_alive()) {
      // Link down: report no flow and resync the baseline so we don't emit a
      // huge bogus delta when telemetry resumes.
      s_flowPrimed = false;
      p = 0;
    } else if (!s_flowPrimed) {
      // First good sample after boot/reconnect: establish baseline only.
      s_flowPrevTotal = total;
      s_flowPrimed = true;
      p = 0;
    } else {
      p = total - s_flowPrevTotal;   // uint32 subtraction handles wrap
      s_flowPrevTotal = total;
    }
    s_flowTotalPulses = total;

    // pulses/sec → L/min: LPM = pps * 60 / (pulses_per_L)
    // pulses_per_L = flowKppl/100
    uint32_t lpm_x10 = (p * 60UL * 1000UL) / settings().flowKppl;  // *10 inherently
    s_lpmX10 = (uint16_t)lpm_x10;

    // Moving average smoothing
    uint8_t nSamples = settings().flowAvgSamples;
    if (nSamples < 1) nSamples = 1;
    if (nSamples > FLOW_AVG_MAX) nSamples = FLOW_AVG_MAX;
    s_flowHistory[s_flowHistIdx % nSamples] = s_lpmX10;
    s_flowHistIdx++;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < nSamples; i++) sum += s_flowHistory[i];
    s_flowSmoothed = (uint16_t)(sum / nSamples);

    // Apply no-flow threshold: if below threshold, report 0
    if (s_flowSmoothed < settings().flowNoFlowThresh) {
      s_flowSmoothed = 0;
    }

    Event e{}; e.type = EV_FLOW_TICK;
    e.p.flow.lpm_x10 = s_flowSmoothed;
    e.p.flow.totalL_x10 = (uint32_t)((uint64_t)s_flowTotalPulses * 1000UL / settings().flowKppl);
    sendEvent(e);
  }
}

uint8_t  sensors_levelPct()       { return s_lastLevel; }
bool     sensors_currentPresent() { return s_currentPresent; }
uint16_t sensors_currentMv()      { return s_lastCurrentMv; }
uint16_t sensors_flowLpmX10()     { return s_flowSmoothed; }  // smoothed + thresholded
// Single source of truth for the "flow present" cutoff used by the state
// machine + safety. Backed by the NVS setting flowNoFlowThresh (editable via
// the "No-Flow Thr" menu item / MQTT). Floored to 1 (0.1 L/min) so a mis-set
// zero can't make "flow present" evaluate true on noise.
uint16_t sensors_flowThreshX10()  { uint16_t t = settings().flowNoFlowThresh; return t < 1 ? 1 : t; }
uint32_t sensors_totalLitersX10() { return (uint32_t)((uint64_t)s_flowTotalPulses * 1000UL / settings().flowKppl); }
bool     sensors_fbOn()           { return s_fbOn; }
bool     sensors_fbOff()          { return s_fbOff; }
int16_t  sensors_tempCx10()       { return s_tempCx10; }
uint16_t sensors_rhX10()          { return s_rhX10; }
bool     sensors_levelPlausible() { return s_levelPlausible; }
float    sensors_pressureMPa()    { return s_pressureMPa; }

uint8_t sensors_pressurePct() {
  float emptyMPa = settings().pressureEmptyMPa1000 / 1000.0f;
  float fullMPa  = settings().pressureFullMPa1000  / 1000.0f;
  if (fullMPa <= emptyMPa) return 0;
  float pct = (s_pressureMPa - emptyMPa) / (fullMPa - emptyMPa) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}
