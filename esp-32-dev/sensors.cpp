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
  #define DHT_PERIOD_MS    2500    // normal poll (sensor minimum is 2 s)
  #define DHT_BACKOFF_MS  30000    // slow poll once the sensor looks dead
  #define DHT_FAIL_LIMIT      5    // consecutive failures before backing off
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
#define CT_AVG_SAMPLES 4
static uint16_t s_lastCurrentMv = 0;
static uint16_t s_ctOffsetMv    = 0;   // measured mid-rail, for diagnostics
static uint16_t s_ctHist[CT_AVG_SAMPLES] = {0};
static uint8_t  s_ctHistIdx = 0;
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
  // AC RMS about the MEASURED mean (auto-zero) using the one-pass variance
  // identity  Vrms = sqrt(mean(x^2) - mean(x)^2).  Deriving the centre from the
  // samples means a bias divider that doesn't sit exactly on ctOffsetMv can't
  // corrupt the reading — a fixed offset error adds in quadrature and would
  // otherwise read as permanent "current present".
  uint32_t sum   = 0;
  uint64_t sumSq = 0;
  uint32_t n     = 0;
  uint32_t t0    = millis();
  while ((uint32_t)(millis() - t0) < CT_SAMPLE_MS) {
    uint32_t mv = analogReadMilliVolts(PIN_CT_ADC);
    sum   += mv;
    sumSq += (uint64_t)mv * mv;
    n++;
  }
  if (n == 0) return 0;

  uint32_t mean   = sum / n;
  uint64_t meanSq = sumSq / n;
  int64_t  var    = (int64_t)meanSq - (int64_t)mean * (int64_t)mean;
  if (var < 0) var = 0;
  s_ctOffsetMv = (uint16_t)mean;
  return (uint16_t)(sqrt((double)var) + 0.5);
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

  // ---- DHT22 (display-only) ----
  // The library bit-bangs this read with interrupts DISABLED, so a marginal
  // sensor can hold them off long enough to trip the 300 ms INT_WDT. Keep the
  // exposure to one transaction per cycle and back off once it starts failing.
#if HAS_DHT22
  static uint32_t lastDht   = 0;
  static uint32_t dhtPeriod = DHT_PERIOD_MS;
  static uint8_t  dhtFails  = 0;
  if (elapsed(lastDht, dhtPeriod)) {
    lastDht = millis();
    // Single attempt: the library caches for 2 s, so an immediate retry returns
    // the same stale failure while still costing interrupts-off time.
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      s_tempCx10 = (int16_t)(t * 10.0f);
      s_rhX10    = (uint16_t)(h * 10.0f);
      if (dhtFails >= DHT_FAIL_LIMIT) Serial.println(F("[SEN] DHT22 recovered"));
      dhtFails  = 0;
      dhtPeriod = DHT_PERIOD_MS;
    } else if (dhtFails < 255) {
      dhtFails++;
      if (dhtFails == DHT_FAIL_LIMIT) {
        dhtPeriod = DHT_BACKOFF_MS;
        Serial.println(F("[SEN] DHT22: persistent read failures, backing off"));
      }
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
    // Smooth over ~1 s: FreeRTOS preemption makes any single 40 ms window a
    // noisy RMS estimate, and switching noise (NeoPixel/buzzer) rides the rail.
    s_ctHist[s_ctHistIdx % CT_AVG_SAMPLES] = sampleCtRmsMv();
    s_ctHistIdx++;
    uint32_t ctSum = 0;
    for (uint8_t i = 0; i < CT_AVG_SAMPLES; i++) ctSum += s_ctHist[i];
    s_lastCurrentMv = (uint16_t)(ctSum / CT_AVG_SAMPLES);
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
uint16_t sensors_currentOffsetMv(){ return s_ctOffsetMv; }
uint16_t sensors_currentAmps()    {
  return (uint16_t)(((uint32_t)s_lastCurrentMv * CT_AMPS_PER_MV_X1000 + 500) / 1000);
}
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
