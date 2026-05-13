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
  MI_SLEEP_TOGGLE,
  MI_SET_CUTOFF_MIN,
  MI_SET_CUTOFF_MAX,
  MI_SET_PRESSURE_MIN,
  MI_SET_PRESSURE_MAX,
  MI_SET_PULSE_ON,
  MI_SET_PULSE_OFF,
  MI_SET_DRYRUN,
  MI_SET_MAX_RUNTIME,
  MI_SET_TIMER1,
  MI_SET_TIMER2,
  MI_SET_CT_THRESH,
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
  "Toggle SLEEP",
  "MIN Water Level",
  "MAX Water Level",
  "Pressure 0%",
  "Pressure 100%",
  "Pulse ON (ms)",
  "Pulse OFF (ms)",
  "Dry-Run (ms)",
  "Max Runtime (s)",
  "Timer 1 (s)",
  "Timer 2 (s)",
  "CT Thresh (mV)",
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

// --- Editor helpers ---
static void startEdit(const char* title, const char* unit,
                      int32_t current, int32_t minV, int32_t maxV, int32_t step,
                      CommitFn fn) {
  s_editing   = true;
  s_editTitle = title;
  s_editUnit  = unit;
  s_editVal   = current;
  s_editMin   = minV;
  s_editMax   = maxV;
  s_editStep  = step;
  s_commitFn  = fn;
  ui_requestUpdate();
}

static void commitCutoffMin(int32_t v) { settings_setU8("cutoffMinPct", (uint8_t)v); }
static void commitCutoffMax(int32_t v) { settings_setU8("cutoffMaxPct", (uint8_t)v); }
static void commitPressMin(int32_t v)  { settings_setU16("pressureEmptyMPa1000", (uint16_t)v); }
static void commitPressMax(int32_t v)  { settings_setU16("pressureFullMPa1000", (uint16_t)v); }
static void commitPulseOn(int32_t v)   { settings_setU32("pulseOnMs", (uint32_t)v); }
static void commitPulseOff(int32_t v)  { settings_setU32("pulseOffMs", (uint32_t)v); }
static void commitDryRun(int32_t v)    { settings_setU32("dryRunMs", (uint32_t)v); }
static void commitMaxRun(int32_t v)    { settings_setU32("maxRuntimeMs", (uint32_t)v * 1000UL); }
static void commitTimer1(int32_t v)    { settings_setU32("timer1Ms", (uint32_t)v * 1000UL); }
static void commitTimer2(int32_t v)    { settings_setU32("timer2Ms", (uint32_t)v * 1000UL); }
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
    case MI_SLEEP_TOGGLE: {
      bool now = !settings().sleepMode;
      settings_setBool("sleepMode", now);
      Event e{}; e.type = EV_MODE_REQ;
      e.p.i32 = now ? MODE_SLEEP : MODE_AUTO;
      sendEvent(e);
      ui_showPopup(now ? "SLEEP: ON" : "SLEEP: OFF");
      break;
    }
    case MI_SET_CUTOFF_MIN:
      startEdit("MIN Water Level", "%", settings().cutoffMinPct, 5, 50, 5, commitCutoffMin);
      return;
    case MI_SET_CUTOFF_MAX:
      startEdit("MAX Water Level", "%", settings().cutoffMaxPct, 50, 100, 5, commitCutoffMax);
      return;
    case MI_SET_PRESSURE_MIN:
      startEdit("Pressure 0%", "mMPa", settings().pressureEmptyMPa1000, 0, 500, 10, commitPressMin);
      return;
    case MI_SET_PRESSURE_MAX:
      startEdit("Pressure 100%", "mMPa", settings().pressureFullMPa1000, 10, 1000, 10, commitPressMax);
      return;
    case MI_SET_PULSE_ON:
      startEdit("Pulse ON", "ms", settings().pulseOnMs, 200, 5000, 100, commitPulseOn);
      return;
    case MI_SET_PULSE_OFF:
      startEdit("Pulse OFF", "ms", settings().pulseOffMs, 200, 5000, 100, commitPulseOff);
      return;
    case MI_SET_DRYRUN:
      startEdit("Dry-Run", "ms", settings().dryRunMs, 5000, 60000, 1000, commitDryRun);
      return;
    case MI_SET_MAX_RUNTIME:
      startEdit("Max Runtime", "s", settings().maxRuntimeMs / 1000, 60, 7200, 60, commitMaxRun);
      return;
    case MI_SET_TIMER1:
      startEdit("Timer 1", "s", settings().timer1Ms / 1000, 30, 3600, 30, commitTimer1);
      return;
    case MI_SET_TIMER2:
      startEdit("Timer 2", "s", settings().timer2Ms / 1000, 30, 3600, 30, commitTimer2);
      return;
    case MI_SET_CT_THRESH:
      startEdit("CT Threshold", "mV", settings().ctThreshMv, 10, 500, 10, commitCtThresh);
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
