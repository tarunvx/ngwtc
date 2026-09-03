#include "menu.h"
#include "config.h"
#include "settings.h"
#include "fault_log.h"
#include "state_machine.h"
#include "event_queue.h"
#include "ui.h"
#include <Arduino.h>

// ============================================================
//  Interactive OLED menu with inline value editor.
//
//  Navigation:  B2=down, B3=up, B4=select/enter, B1=back
//  Value edit:  B3=increment, B2=decrement, B4=confirm, B1=cancel
// ============================================================

// --- Menu items ---
enum MenuItem : uint8_t {
  MI_MODE_AUTO = 0,
  MI_MODE_MANUAL,
  MI_MODE_TIMER,
  MI_MODE_MD1,
  MI_SLEEP_TOGGLE,
  MI_SMART_SENSE,
  MI_OVERFLOW_ONCE,
  MI_LED_SOURCE,
  MI_DIAG_MODE,
  MI_SILENT_MODE,
  MI_IGNORE_ACTUATION,
  MI_SILENT_ON_OFFFB,
  MI_BYPASS_CURRENT,
  MI_BYPASS_FLOW,
  MI_BYPASS_FEEDBACK,
  MI_SET_CUTOFF_MIN,
  MI_SET_CUTOFF_MAX,
  MI_SET_LED_ALT,
  MI_SET_UI_DIM,
  MI_SET_FLOW_THRESH,
  MI_SET_PULSE_ON,
  MI_SET_PULSE_OFF,
  MI_SET_DRYRUN,
  MI_SET_MAX_RUNTIME,
  MI_SET_TIMER1,
  MI_SET_TIMER2,
  MI_SET_CT_THRESH,
  MI_SET_CT_CAL,
  MI_SHOW_UPTIME,
  MI_RESET_FAULTS,
  MI_SHOW_FAULTS,
  MI_DEFAULTS,
  MI_REBOOT,
  MI_EXIT,
  MI_COUNT
};

static const char* kLabels[MI_COUNT] = {
  "Mode: AUTO",
  "Mode: MANUAL",
  "Mode: TIMER",
  "Mode: MD1",
  "Toggle SLEEP",
  "Smart-Sense",
  "Ignore Full 1x",
  "LED: Flt/US",
  "Diagnostics",
  "Silent Mode",
  "MD1: No Actuate",
  "MD1: Stop on FB",
  "Bypass I-Sense",
  "Bypass Flow",
  "Bypass Feedback",
  "MIN Water Level",
  "MAX Water Level",
  "LED Alt (ms)",
  "Screen Dim (s)",
  "Flow Threshold",
  "Pulse ON (ms)",
  "Pulse OFF (ms)",
  "Dry-Run (ms)",
  "Max Runtime (s)",
  "Timer 1 (s)",
  "Timer 2 (s)",
  "CT Thresh (mV)",
  "CT Calib (A)",
  "Show Uptime",
  "Clear Faults",
  "Show Faults",
  "Reset Defaults",
  "Reboot",
  "Exit",
};

// --- State ---
static bool    s_open = false;
static uint8_t s_sel  = 0;

// --- Value editor state ---
static bool    s_editing = false;
static const char* s_editTitle = "";
static const char* s_editUnit  = "";
static int32_t s_editVal = 0;
static int32_t s_editMin = 0;
static int32_t s_editMax = 100;
static int32_t s_editStep = 1;
static bool    s_editDecimal = false;   // render as value/10 with one decimal

// Function pointer to commit the edited value
typedef void (*CommitFn)(int32_t val);
static CommitFn s_commitFn = nullptr;

void menu_init() {
  s_open = false;
  s_sel  = 0;
  s_editing = false;
}

bool menu_isOpen()    { return s_open; }
bool menu_isEditing() { return s_editing; }

uint8_t menu_itemCount()              { return MI_COUNT; }
const char* menu_itemLabel(uint8_t i) { return (i < MI_COUNT) ? kLabels[i] : ""; }
uint8_t menu_selectedIndex()          { return s_sel; }

const char* menu_editTitle() { return s_editTitle; }
int32_t     menu_editValue() { return s_editVal; }
const char* menu_editUnit()  { return s_editUnit; }
bool        menu_editIsDecimal() { return s_editDecimal; }

// --- Editor helpers ---
static void startEdit(const char* title, const char* unit,
                      int32_t current, int32_t minV, int32_t maxV, int32_t step,
                      CommitFn fn, bool decimal = false) {
  s_editing   = true;
  s_editTitle = title;
  s_editUnit  = unit;
  s_editDecimal = decimal;
  s_editVal   = current;
  s_editMin   = minV;
  s_editMax   = maxV;
  s_editStep  = step;
  s_commitFn  = fn;
  ui_requestUpdate();
}

static void commitCutoffMin(int32_t v) { settings_setU8("cutoffMinPct", (uint8_t)v); }
static void commitCutoffMax(int32_t v) { settings_setU8("cutoffMaxPct", (uint8_t)v); }
static void commitLedAlt(int32_t v)    { settings_setU16("ledAltMs", (uint16_t)v); }
static void commitUiDim(int32_t v)     { settings_setU16("uiDimMs", (uint16_t)(v * 1000)); }
static void commitFlowThresh(int32_t v){ settings_setU16("flowNoFlowThresh", (uint16_t)v); }
static void commitPulseOn(int32_t v)   { settings_setU32("pulseOnMs", (uint32_t)v); }
static void commitPulseOff(int32_t v)  { settings_setU32("pulseOffMs", (uint32_t)v); }
static void commitDryRun(int32_t v)    { settings_setU16("dryRunSec",     (uint16_t)v); }
static void commitMaxRun(int32_t v)    { settings_setU16("maxRuntimeMin", (uint16_t)v); }
static void commitTimer1(int32_t v)    { settings_setU16("timer1Sec",     (uint16_t)v); }
static void commitTimer2(int32_t v)    { settings_setU16("timer2Sec",     (uint16_t)v); }
static void commitCtCal(int32_t v)     { settings_setU8 ("ctCalAmps", (uint8_t)(int8_t)v); }
static void commitCtThresh(int32_t v)  { settings_setU16("ctThreshMv", (uint16_t)v); }

// --- Activate menu item ---
static void activate() {
  char popBuf[32];
  switch ((MenuItem)s_sel) {
    case MI_MODE_AUTO: {
      Event e{}; e.type = EV_MODE_REQ; e.p.i32 = MODE_AUTO; sendEvent(e);
      ui_showPopup("Mode: AUTO");
      break;
    }
    case MI_MODE_MANUAL: {
      Event e{}; e.type = EV_MODE_REQ; e.p.i32 = MODE_MANUAL; sendEvent(e);
      ui_showPopup("Mode: MANUAL");
      break;
    }
    case MI_MODE_TIMER: {
      Event e{}; e.type = EV_MODE_REQ; e.p.i32 = MODE_TIMER; sendEvent(e);
      ui_showPopup("Mode: TIMER");
      break;
    }
    case MI_MODE_MD1: {
      Event e{}; e.type = EV_MODE_REQ; e.p.i32 = MODE_MD1; sendEvent(e);
      ui_showPopup("Mode: MD1");
      break;
    }
    case MI_SILENT_MODE: {
      bool on = !settings().silentMode;
      settings_setBool("silentMode", on);
      ui_showPopup(on ? "Silent: ON" : "Silent: OFF");
      break;
    }
    case MI_IGNORE_ACTUATION: {
      bool on = !settings().ignoreActuation;
      settings_setBool("ignoreActuation", on);
      ui_showPopup(on ? "MD1 Actuate: NO" : "MD1 Actuate: YES");
      break;
    }
    case MI_SILENT_ON_OFFFB: {
      bool on = !settings().silentBzrOnOffFB;
      settings_setBool("silentBzrOnOffFB", on);
      ui_showPopup(on ? "Stop on FB: ON" : "Stop on FB: OFF");
      break;
    }
    case MI_SLEEP_TOGGLE: {
      bool now = !settings().sleepMode;
      settings_setBool("sleepMode", now);
      Event e{}; e.type = EV_MODE_REQ;
      e.p.i32 = now ? MODE_SLEEP : MODE_AUTO;
      sendEvent(e);
      ui_showPopup(now ? "SLEEP: ON" : "SLEEP: OFF");
      break;
    }
    case MI_SMART_SENSE: {
      bool now = !settings().smartSense;
      settings_setBool("smartSense", now);
      ui_showPopup(now ? "SmartSns: ON" : "SmartSns: OFF");
      break;
    }
    case MI_OVERFLOW_ONCE: {
      // One-shot: arm the overflow override so the next MANUAL/TIMER run may
      // run past full. Self-clears on stop / arm timeout. AUTO unaffected.
      bool on = sm_toggleOverflowIgnore();
      ui_showPopup(on ? "Overflow ARM 1x" : "Overflow OFF");
      break;
    }
    case MI_LED_SOURCE: {
      bool wasUs = (settings().levelSource == LVL_ULTRASONIC);
      settings_setU8("levelSource", wasUs ? LVL_FLOAT : LVL_ULTRASONIC);
      ui_showPopup(wasUs ? "LED: Floats" : "LED: Ultrasonic");
      break;
    }
    case MI_DIAG_MODE: {
      bool on = !settings().diagMode;
      ui_setDiagMode(on);
      ui_showPopup(on ? "Diag: ON" : "Diag: OFF");
      if (on) s_open = false;   // drop straight to the monitor
      break;
    }
    case MI_BYPASS_CURRENT: {
      bool now = !settings().bypassCurrentSense;
      settings_setBool("bypassCurrentSense", now);
      ui_showPopup(now ? "I-Bypass: ON" : "I-Bypass: OFF");
      if (now) Serial.println("[MENU] WARNING: Current sense bypassed!");
      break;
    }
    case MI_BYPASS_FLOW: {
      bool now = !settings().bypassFlowSense;
      settings_setBool("bypassFlowSense", now);
      ui_showPopup(now ? "F-Bypass: ON" : "F-Bypass: OFF");
      if (now) Serial.println("[MENU] WARNING: Flow sense bypassed!");
      break;
    }
    case MI_BYPASS_FEEDBACK: {
      bool now = !settings().bypassFeedback;
      settings_setBool("bypassFeedback", now);
      ui_showPopup(now ? "FB-Bypass: ON" : "FB-Bypass: OFF");
      if (now) Serial.println("[MENU] WARNING: Feedback bypassed!");
      break;
    }
    case MI_SET_CUTOFF_MIN:
      startEdit("MIN Water Level", "%", settings().cutoffMinPct, 5, 50, 5, commitCutoffMin);
      return;
    case MI_SET_CUTOFF_MAX:
      startEdit("MAX Water Level", "%", settings().cutoffMaxPct, 50, 100, 5, commitCutoffMax);
      return;
    case MI_SET_LED_ALT:
      startEdit("LED Alt", "ms", settings().ledAltMs, 500, 10000, 500, commitLedAlt);
      return;
    case MI_SET_UI_DIM: {
      uint16_t v = settings().uiDimMs;
      if (v < 5000 || v > 60000) v = DEF_UI_DIM_MS;   // legacy/unset NVS slot
      startEdit("Screen Dim", "s", v / 1000, 5, 60, 5, commitUiDim);
      return;
    }
    case MI_SET_FLOW_THRESH:
      // Shown and edited in L/min with one decimal; stored as lpm x10, so a
      // step of 2 is 0.2 L/min.
      startEdit("No-Flow Thr", "L/min", settings().flowNoFlowThresh, 0, 200, 2, commitFlowThresh, true);
      return;
    case MI_SET_PULSE_ON:
      startEdit("Pulse ON", "ms", settings().pulseOnMs, 200, 5000, 100, commitPulseOn);
      return;
    case MI_SET_PULSE_OFF:
      startEdit("Pulse OFF", "ms", settings().pulseOffMs, 200, 5000, 100, commitPulseOff);
      return;
    case MI_SET_DRYRUN:
      startEdit("Dry-Run", "s", settings().dryRunSec, 5, 120, 1, commitDryRun);
      return;
    case MI_SET_MAX_RUNTIME:
      startEdit("Max Runtime", "min", settings().maxRuntimeMin, 1, 240, 1, commitMaxRun);
      return;
    case MI_SET_TIMER1:
      startEdit("Timer 1", "s", settings().timer1Sec, 30, 3600, 30, commitTimer1);
      return;
    case MI_SET_TIMER2:
      startEdit("Timer 2", "s", settings().timer2Sec, 30, 3600, 30, commitTimer2);
      return;
    case MI_SET_CT_THRESH:
      startEdit("CT Threshold", "mV", settings().ctThreshMv, 10, 500, 10, commitCtThresh);
      return;
    case MI_SET_CT_CAL:
      startEdit("CT Calib", "A", settings().ctCalAmps, -20, 20, 1, commitCtCal);
      return;
    case MI_SHOW_UPTIME: {
      uint32_t up = millis()/1000;
      snprintf(popBuf, sizeof(popBuf), "%luh%02lum%02lus", up/3600, (up/60)%60, up%60);
      ui_showPopup(popBuf, 3000);
      s_open = true;
      return;
    }
    case MI_RESET_FAULTS:
      faultlog_clear();
      sm_clearLatched();
      ui_showPopup("Faults CLR");
      break;
    case MI_SHOW_FAULTS:
      faultlog_dump(Serial);
      ui_showPopup("See Serial");
      s_open = true;
      return;
    case MI_DEFAULTS:
      settings_resetDefaults();
      ui_showPopup("Defaults OK");
      break;
    case MI_REBOOT:
      ui_showPopup("Rebooting..");
      delay(500);
      ESP.restart();
      break;
    case MI_EXIT:
    default:
      break;
  }
  s_open = false;
  ui_requestUpdate();
}

// --- Button handler ---
void menu_handleButton(ButtonId id, PressKind k) {
  // --- Value editor mode ---
  if (s_editing) {
    if (k != PRESS_SHORT) return;
    switch (id) {
      case BTN3: // increment
        s_editVal += s_editStep;
        if (s_editVal > s_editMax) s_editVal = s_editMax;
        break;
      case BTN2: // decrement
        s_editVal -= s_editStep;
        if (s_editVal < s_editMin) s_editVal = s_editMin;
        break;
      case BTN4: { // confirm
        if (s_commitFn) s_commitFn(s_editVal);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s set: %ld%s", s_editTitle, (long)s_editVal, s_editUnit);
        s_editing = false;
        s_open = false;
        ui_showPopup(buf, 2000);
        Serial.printf("[MENU] %s\n", buf);
        return;
      }
      case BTN1: // cancel
        s_editing = false;
        // stay in menu
        break;
    }
    ui_requestUpdate();
    return;
  }

  // --- Open menu ---
  if (!s_open) {
    if (id == BTN4 && k == PRESS_SHORT) {
      s_open = true; s_sel = 0;
      Serial.println("[MENU] opened");
      ui_requestUpdate();
    }
    return;
  }

  // --- Menu navigation ---
  if (k == PRESS_SHORT) {
    switch (id) {
      case BTN1:
        s_open = false;
        Serial.println("[MENU] back/close");
        break;
      case BTN3:
        s_sel = (s_sel + 1) % MI_COUNT;
        break;
      case BTN2:
        s_sel = (s_sel + MI_COUNT - 1) % MI_COUNT;
        break;
      case BTN4:
        activate();
        return;
    }
  } else { // PRESS_LONG
    if (id == BTN1 || id == BTN4) {
      s_open = false;
      Serial.println("[MENU] closed (long)");
    }
  }
  ui_requestUpdate();
}
