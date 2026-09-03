#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include "config.h"

enum Mode : uint8_t {
  MODE_AUTO = 0, MODE_MANUAL = 1, MODE_TIMER = 2, MODE_SLEEP = 3, MODE_MAINTENANCE = 4,
  // MD1: pump is started ONLY by the physical starter. Firmware watches the
  // feedback switches, alarms at 100%, and (unless ignoreActuation) issues the
  // OFF stroke. It never starts the pump.
  MODE_MD1 = 5,
};

struct Settings {
  // cutoff levels (percent)
  uint8_t  cutoffMinPct;   // triggers actuation (default 25)
  uint8_t  cutoffMaxPct;   // triggers buzzer/stop (default 100)
  // calibration / timing
  // Units here are the ones a USER edits. Anything the firmware compares
  // against millis() is converted at the use site via the helpers below.
  uint32_t pulseOnMs;
  uint32_t pulseOffMs;
  uint16_t dryRunSec;       // was dryRunMs
  uint32_t feedbackMs;
  uint32_t currentMs;
  uint32_t currentOffMs;
  uint32_t flowOffMs;
  uint16_t maxRuntimeMin;   // was maxRuntimeMs
  uint16_t timer1Sec;       // was timer1Ms
  uint16_t timer2Sec;       // was timer2Ms

  // sensor cal
  uint16_t flowKppl;        // pulses per litre (YF-S201 ≈ 450)
  uint16_t ctOffsetMv;
  uint16_t ctThreshMv;
  int8_t   ctCalAmps;       // +/- trim applied to the displayed/derived amps
  uint8_t  levelHystPct;

  // mode + flags
  uint8_t  mode;            // Mode
  bool     sleepMode;       // explicit sleep flag (separate from mode for safety)
  bool     hwTimerPresent;
  bool     adaptiveDryRun;
  // Renamed in place from the dormant mcuUpsPresent slot — same type/offset, so
  // sizeof(Settings) and the stored NVS blob stay compatible.
  bool     diagMode;        // continuous diagnostic screen instead of the dashboard
  uint8_t  levelSource;     // LevelSource

  // v6: the pressure sensor was replaced by the ultrasonic, so this slot was
  // repurposed in place (same type/offset => NVS blob stays compatible).
  uint16_t ledAltMs;              // LED bar state<->level alternation period (ms)
  uint16_t uiDimMs;               // OLED idle time before dimming (ms)

  // Sensor bypass flags (for unreliable sensors during bring-up)
  bool     bypassCurrentSense;    // skip current-sense checks during actuation
  bool     bypassFlowSense;       // skip flow-sense checks during actuation
  bool     bypassFeedback;        // skip feedback micro-switch checks during actuation

  // Smart-Sense: auto-detect external pump/filling activity
  bool     smartSense;            // when true, monitor sensors for external activity

  // MD1 / buzzer policy
  bool     ignoreActuation;       // MD1: alarm only, never drive the OFF stroke
  bool     silentBzrOnOffFB;      // MD1: cancel the full-tank alarm on feedback-OFF
  bool     silentMode;            // global mute except FAULT_LATCHED

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
const char* modeNameShort(uint8_t m);   // 3 chars, for the OLED

// User-facing units -> millis. Every millis() comparison goes through these.
static inline uint32_t settings_dryRunMs()     { return (uint32_t)settings().dryRunSec  * 1000UL; }
static inline uint32_t settings_maxRuntimeMs() { return (uint32_t)settings().maxRuntimeMin * 60000UL; }
static inline uint32_t settings_timer1Ms()     { return (uint32_t)settings().timer1Sec  * 1000UL; }
static inline uint32_t settings_timer2Ms()     { return (uint32_t)settings().timer2Sec  * 1000UL; }

#endif
