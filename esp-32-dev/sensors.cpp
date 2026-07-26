#include "sensors.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include "events.h"
#include "event_queue.h"
#include "time_utils.h"
#include "ui.h"

#if HAS_DHT22
  #include <DHT.h>
  static DHT dht(PIN_DHT22, DHT22);
#endif

// ============== Pressure sensor ================
#define PIN_PRESSURE_ADC 34
static float s_pressureMPa = 0.0f;

// ============== flow ISR ===================
#if HAS_FLOW_ISR
static volatile uint32_t s_flowPulses = 0;

static void IRAM_ATTR flowIsr() {
  s_flowPulses++;
}
#endif

static uint32_t s_flowLastAggMs = 0;
static uint32_t s_flowTotalPulses = 0;
static uint16_t s_lpmX10 = 0;

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
  pinMode(PIN_FLOAT_25,  INPUT_PULLUP);
  pinMode(PIN_FLOAT_50,  INPUT_PULLUP);
  pinMode(PIN_FLOAT_75,  INPUT_PULLUP);
  pinMode(PIN_FLOAT_100, INPUT_PULLUP);

  pinMode(PIN_FB_ON,  INPUT_PULLUP);
  pinMode(PIN_FB_OFF, INPUT_PULLUP);

  pinMode(PIN_FLOW, INPUT_PULLUP);
#if HAS_FLOW_ISR
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowIsr, FALLING);
#endif

#if HAS_CT_CLAMP
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CT_ADC, ADC_11db);
#endif

#if HAS_DHT22
  dht.begin();
  Serial.println(F("[SEN] DHT22 init"));
#endif

  // Pressure sensor ADC (input-only pin, no setup needed beyond resolution)
  analogReadResolution(12);
}

static uint8_t readFloats() {
  // Active LOW — water present grounds the input.
  bool l25  = (digitalRead(PIN_FLOAT_25)  == LOW);
  bool l50  = (digitalRead(PIN_FLOAT_50)  == LOW);
  bool l75  = (digitalRead(PIN_FLOAT_75)  == LOW);
  bool l100 = (digitalRead(PIN_FLOAT_100) == LOW);

  // Plausibility: monotonic — if L100 high, all lower must be high.
  s_levelPlausible = !((l100 && (!l75 || !l50 || !l25)) ||
                       (l75  && (!l50 || !l25)) ||
                       (l50  && !l25));

  if (l100) return 100;
  if (l75)  return 75;
  if (l50)  return 50;
  if (l25)  return 25;
  return 0;
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
  // ---- pressure sensor (every 1s) ----
  static uint32_t lastPressure = 0;
  if (elapsed(lastPressure, 1000)) {
    lastPressure = millis();
    int raw = analogRead(PIN_PRESSURE_ADC);
    float v = raw * (3.3f / 4095.0f);
    // Sensor: 0.5V = 0 MPa, 4.5V = 1 MPa
    // Note: ESP32 ADC max is 3.3V, so with a voltage divider or
    // attenuation you may need to scale. For now, direct 3.3V read.
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
  // Floats are discrete (0/25/50/75/100), so emit on any change.
  // (Previous code had a dead hysteresis guard whose `|| lvl != s_lastLevel`
  // made the outer test always true — levelHystPct never gated anything.
  // Simplified to a plain change-detect; hysteresis on discrete floats is a
  // no-op anyway.)
  uint8_t lvl = readFloats();
  if (lvl != s_lastLevel) {
    s_lastLevel = lvl;
    Event e{}; e.type = EV_LEVEL_CHANGED; e.p.i32 = lvl;
    sendEvent(e);
    ui_requestUpdate(); // Ensure UI updates immediately on level change
  }

  if (!s_levelPlausible) {
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

  // ---- flow aggregation ----
#if HAS_FLOW_ISR
  if (elapsed(s_flowLastAggMs, 1000)) {
    s_flowLastAggMs = millis();
    noInterrupts();
    uint32_t p = s_flowPulses;
    s_flowPulses = 0;
    interrupts();

    s_flowTotalPulses += p;
    // pulses/sec → L/min for a YF-S201: LPM = pps * 60 / pulses_per_litre,
    // where flowKppl IS the pulses-per-litre calibration (YF-S201 ≈ 450, i.e.
    // F = 7.5·Q). Scaled ×10 for the integer result: lpm_x10 = pps * 600 / flowKppl.
    uint32_t lpm_x10 = (p * 60UL * 10UL) / settings().flowKppl;
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
    e.p.flow.totalL_x10 = (uint32_t)((uint64_t)s_flowTotalPulses * 10UL / settings().flowKppl);
    sendEvent(e);
  }
#endif
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
uint32_t sensors_totalLitersX10() { return (uint32_t)((uint64_t)s_flowTotalPulses * 10UL / settings().flowKppl); }
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
