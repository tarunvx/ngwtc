#include "settings.h"
#include <Preferences.h>

#define NVS_NS  "swtc"
#define MAGIC   0x5A11

static Settings s_cfg;
static Preferences s_prefs;

static void loadDefaults() {
  s_cfg.cutoffMinPct    = 25;
  s_cfg.cutoffMaxPct    = 100;
  s_cfg.pulseOnMs       = DEF_PULSE_ON_MS;
  s_cfg.pulseOffMs      = DEF_PULSE_OFF_MS;
  s_cfg.dryRunMs        = DEF_DRYRUN_MS;
  s_cfg.feedbackMs      = DEF_FEEDBACK_MS;
  s_cfg.currentMs       = DEF_CURRENT_MS;
  s_cfg.currentOffMs    = DEF_CURRENT_OFF_MS;
  s_cfg.flowOffMs       = DEF_FLOW_OFF_MS;
  s_cfg.maxRuntimeMs    = DEF_MAX_RUNTIME_MS;
  s_cfg.timer1Ms        = DEF_TIMER1_MS;
  s_cfg.timer2Ms        = DEF_TIMER2_MS;
  s_cfg.flowKppl        = DEF_FLOW_KPPL;
  s_cfg.ctOffsetMv      = DEF_CT_OFFSET_MV;
  s_cfg.ctThreshMv      = DEF_CT_THRESH_MV;
  s_cfg.levelHystPct    = DEF_LEVEL_HYST;
  s_cfg.mode            = MODE_AUTO;
  s_cfg.sleepMode       = false;
  s_cfg.hwTimerPresent  = DEFAULT_HW_TIMER_PRESENT;
  s_cfg.adaptiveDryRun  = DEFAULT_ADAPTIVE_DRYRUN;
  s_cfg.mcuUpsPresent   = DEFAULT_MCU_UPS_PRESENT;
  s_cfg.levelSource     = DEFAULT_LEVEL_SOURCE;
  s_cfg.pressureEmptyMPa1000 = 0;     // 0 MPa = empty
  s_cfg.pressureFullMPa1000  = 100;   // 0.100 MPa = full (adjust per install)
  s_cfg.magic           = MAGIC;
}

void settings_init() {
  s_prefs.begin(NVS_NS, /*ro=*/false);
  loadDefaults();
  size_t got = s_prefs.getBytes("blob", &s_cfg, sizeof(s_cfg));
  if (got != sizeof(s_cfg) || s_cfg.magic != MAGIC) {
    Serial.println(F("[CFG] no/invalid NVS blob — defaults"));
    loadDefaults();
    settings_save();
  } else {
    Serial.println(F("[CFG] loaded from NVS"));
  }
}

void settings_save() {
  s_cfg.magic = MAGIC;
  s_prefs.putBytes("blob", &s_cfg, sizeof(s_cfg));
}

const Settings& settings() { return s_cfg; }

void settings_resetDefaults() { loadDefaults(); settings_save(); }

// Generic setter helpers — string match keys to fields.
static bool _matchU32(const char* k, const char* want, uint32_t& field, uint32_t v) {
  if (strcmp(k, want) == 0) { field = v; return true; }
  return false;
}
static bool _matchU16(const char* k, const char* want, uint16_t& field, uint16_t v) {
  if (strcmp(k, want) == 0) { field = v; return true; }
  return false;
}
static bool _matchU8(const char* k, const char* want, uint8_t& field, uint8_t v) {
  if (strcmp(k, want) == 0) { field = v; return true; }
  return false;
}
static bool _matchBool(const char* k, const char* want, bool& field, bool v) {
  if (strcmp(k, want) == 0) { field = v; return true; }
  return false;
}

bool settings_setU32(const char* k, uint32_t v) {
  bool ok =
       _matchU32(k, "pulseOnMs",    s_cfg.pulseOnMs,    v)
    || _matchU32(k, "pulseOffMs",   s_cfg.pulseOffMs,   v)
    || _matchU32(k, "dryRunMs",     s_cfg.dryRunMs,     v)
    || _matchU32(k, "feedbackMs",   s_cfg.feedbackMs,   v)
    || _matchU32(k, "currentMs",    s_cfg.currentMs,    v)
    || _matchU32(k, "currentOffMs", s_cfg.currentOffMs, v)
    || _matchU32(k, "flowOffMs",    s_cfg.flowOffMs,    v)
    || _matchU32(k, "maxRuntimeMs", s_cfg.maxRuntimeMs, v)
    || _matchU32(k, "timer1Ms",     s_cfg.timer1Ms,     v)
    || _matchU32(k, "timer2Ms",     s_cfg.timer2Ms,     v);
  if (ok) settings_save();
  return ok;
}

bool settings_setU16(const char* k, uint16_t v) {
  bool ok =
       _matchU16(k, "flowKppl",   s_cfg.flowKppl,   v)
    || _matchU16(k, "ctOffsetMv", s_cfg.ctOffsetMv, v)
    || _matchU16(k, "ctThreshMv", s_cfg.ctThreshMv, v)
    || _matchU16(k, "pressureEmptyMPa1000", s_cfg.pressureEmptyMPa1000, v)
    || _matchU16(k, "pressureFullMPa1000",  s_cfg.pressureFullMPa1000,  v);
  if (ok) settings_save();
  return ok;
}

bool settings_setU8(const char* k, uint8_t v) {
  bool ok =
       _matchU8(k, "levelHystPct", s_cfg.levelHystPct, v)
    || _matchU8(k, "mode",         s_cfg.mode,         v)
    || _matchU8(k, "levelSource",  s_cfg.levelSource,  v)
    || _matchU8(k, "cutoffMinPct", s_cfg.cutoffMinPct, v)
    || _matchU8(k, "cutoffMaxPct", s_cfg.cutoffMaxPct, v);
  if (ok) settings_save();
  return ok;
}

bool settings_setBool(const char* k, bool v) {
  bool ok =
       _matchBool(k, "sleepMode",      s_cfg.sleepMode,      v)
    || _matchBool(k, "hwTimerPresent", s_cfg.hwTimerPresent, v)
    || _matchBool(k, "adaptiveDryRun", s_cfg.adaptiveDryRun, v)
    || _matchBool(k, "mcuUpsPresent",  s_cfg.mcuUpsPresent,  v);
  if (ok) settings_save();
  return ok;
}

const char* modeName(uint8_t m) {
  switch (m) {
    case MODE_AUTO:        return "AUTO";
    case MODE_MANUAL:      return "MANUAL";
    case MODE_TIMER:       return "TIMER";
    case MODE_SLEEP:       return "SLEEP";
    case MODE_MAINTENANCE: return "MAINT";
    default:               return "?";
  }
}
