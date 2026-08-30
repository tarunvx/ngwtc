#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include "config.h"

enum Mode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1, MODE_TIMER = 2, MODE_SLEEP = 3, MODE_MAINTENANCE = 4 };

struct Settings {
  // cutoff levels (percent)
  uint8_t  cutoffMinPct;   // triggers actuation (default 25)
  uint8_t  cutoffMaxPct;   // triggers buzzer/stop (default 100)
  // calibration / timing
  uint32_t pulseOnMs;
  uint32_t pulseOffMs;
  uint32_t dryRunMs;
  uint32_t feedbackMs;
  uint32_t currentMs;
  uint32_t currentOffMs;
  uint32_t flowOffMs;
  uint32_t maxRuntimeMs;
  uint32_t timer1Ms;
  uint32_t timer2Ms;

  // sensor cal
  uint16_t flowKppl;        // pulses per litre (YF-S201 ≈ 450)
  uint16_t ctOffsetMv;
  uint16_t ctThreshMv;
  uint8_t  levelHystPct;

  // mode + flags
  uint8_t  mode;            // Mode
  bool     sleepMode;       // explicit sleep flag (separate from mode for safety)
  bool     hwTimerPresent;
  bool     adaptiveDryRun;
  bool     mcuUpsPresent;
  uint8_t  levelSource;     // LevelSource

  // v6: the pressure sensor was replaced by the ultrasonic, so this slot was
  // repurposed in place (same type/offset => NVS blob stays compatible).
  uint16_t ledAltMs;              // LED bar state<->level alternation period (ms)
  uint16_t pressureFullMPa1000;   // reserved (unused in v6)

  // Sensor bypass flags (for unreliable sensors during bring-up)
  bool     bypassCurrentSense;    // skip current-sense checks during actuation
  bool     bypassFlowSense;       // skip flow-sense checks during actuation
  bool     bypassFeedback;        // skip feedback micro-switch checks during actuation

  // Smart-Sense: auto-detect external pump/filling activity
  bool     smartSense;            // when true, monitor sensors for external activity

  // Flow sensor tuning
  uint16_t flowNoFlowThresh;      // lpm_x10 below this = "no flow" (default 5 = 0.5 L/min)
  uint8_t  flowAvgSamples;        // number of 1-second samples to average (default 4)

  uint16_t magic;           // sanity
};

void     settings_init();              // load from NVS or defaults
const Settings& settings();
bool     settings_setU32(const char* key, uint32_t v);
bool     settings_setU16(const char* key, uint16_t v);
bool     settings_setU8 (const char* key, uint8_t  v);
bool     settings_setBool(const char* key, bool v);
void     settings_save();              // persist current snapshot
void     settings_resetDefaults();
const char* modeName(uint8_t m);

#endif
