#include "sensors.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include "events.h"
#include "event_queue.h"
#include "time_utils.h"
#include "ui.h"
#include "link.h"   // v6: floats/level/flow arrive over ESP-NOW

#if HAS_DHT22
  #include <DHT.h>
  static DHT dht(PIN_DHT22, DHT22);
  #define DHT_PERIOD_MS    2500    // normal poll (sensor minimum is 2 s)
  #define DHT_BACKOFF_MS  30000    // slow poll once the sensor looks dead
  #define DHT_FAIL_LIMIT      5    // consecutive failures before backing off
#endif

// ============== Ultrasonic level (v6: sourced from tank node) ====
static uint16_t s_distanceMm = 0;
static uint8_t  s_usLevelPct = 0;

// ============== Flow (v6: cumulative pulses from tank node) =====
static uint32_t s_flowLastAggMs   = 0;
static uint32_t s_flowTotalPulses = 0;   // mirrors tank-node cumulative count
static uint32_t s_flowPrevTotal   = 0;   // previous cumulative for 1s delta
static bool     s_flowPrimed      = false;
static uint16_t s_lpmX10          = 0;


// Moving average buffer for flow smoothing
#define FLOW_AVG_MAX 8
// Ceiling for one 1 s aggregation window (~133 L/min) — far above the
// YF-S201's 30 L/min range, so only glitches exceed it.
#define FLOW_MAX_PULSES_PER_WINDOW 1000
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
  // v6: floats, ultrasonic level and flow are NOT wired to this node anymore —
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

  Serial.println(F("[SEN] v6: floats/level/flow sourced from tank node via link"));
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
  // ---- ultrasonic level (every 1s, sourced from tank node) ----
  // The tank node owns the distance->percent mapping (its two calibration
  // constants), so we just mirror what it sends.
  static uint32_t lastUs = 0;
  if (elapsed(lastUs, 1000)) {
    lastUs = millis();
    s_distanceMm = link_distanceMm();
    s_usLevelPct = link_usLevelPct();
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
  // Edge-triggered: the condition persists for as long as the wiring is wrong,
  // and re-raising it every tick floods the event queue and fills the 32-slot
  // fault ring with a single code, hiding everything else.
  static bool s_implausibleLatched = false;
  bool implausible = link_alive() && !s_levelPlausible;
  if (implausible && !s_implausibleLatched) {
    Event e{}; e.type = EV_FAULT;
    e.p.fault = { FC_IMPLAUSIBLE_LEVEL, SEV_WARN };
    sendEvent(e);
  }
  s_implausibleLatched = implausible;

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
    } else if (total < s_flowPrevTotal) {
      // Tank node restarted: its cumulative counter reset to 0, so a plain
      // subtraction would underflow into a huge bogus delta. Re-baseline.
      s_flowPrevTotal = total;
      p = 0;
    } else {
      p = total - s_flowPrevTotal;
      s_flowPrevTotal = total;
      // A YF-S201 tops out near 225 pulses/s, so anything far above that is a
      // noise burst or a missed restart rather than water.
      if (p > FLOW_MAX_PULSES_PER_WINDOW) p = 0;
    }
    s_flowTotalPulses = total;

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
uint16_t sensors_distanceMm()          { return s_distanceMm; }
uint8_t  sensors_ultrasonicLevelPct()  { return s_usLevelPct; }
