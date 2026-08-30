#include "state_machine.h"
#include "config.h"
#include "settings.h"
#include "sensors.h"
#include "actuator.h"
#include "fault_log.h"
#include "event_queue.h"
#include "time_utils.h"
#include "menu.h"
#include "ui.h"
#include "buzzer.h"
#include "link.h"   // v6: link liveness gates AUTO + forces safe-stop

static SystemState s_state = ST_BOOT_SELFTEST;
static SystemState s_prev  = ST_BOOT_SELFTEST;
static uint32_t    s_enteredAt = 0;
static uint32_t    s_pumpStartedAt = 0;

// Sub-phase tracking for STARTING / STOPPING
static uint32_t s_phaseStart = 0;
static bool     s_sawFb = false;
static bool     s_sawCurrent = false;
static bool     s_sawFlow = false;

// Timer state
static uint32_t s_timerDeadline = 0;
static bool     s_timerActive = false;
static uint8_t  s_timerWhich = 0;

// One-shot overflow override (see state_machine.h). Volatile RAM only — armed
// by B2-long / menu / MQTT, lets a MANUAL/TIMER run deliberately continue past
// the 100% cutoff, then self-clears on the next stop.
static bool     s_overflowIgnore  = false;
static uint32_t s_overflowArmedAt = 0;
#define OVERFLOW_ARM_TIMEOUT_MS 60000UL   // auto-disarm if armed but no run starts

// Fault repeat counter
struct RepeatCtr { uint32_t firstAt; uint8_t count; FaultCode last; };
static RepeatCtr s_rep = {0, 0, FC_NONE};

const char* sm_stateName(SystemState s) {
  switch (s) {
    case ST_BOOT_SELFTEST: return "BOOT_SELFTEST";
    case ST_IDLE:          return "IDLE";
    case ST_AUTO_FILLING:  return "AUTO_FILLING";
    case ST_MANUAL_ON:     return "MANUAL_ON";
    case ST_TIMER_RUNNING: return "TIMER_RUNNING";
    case ST_STARTING:      return "STARTING";
    case ST_STOPPING:      return "STOPPING";
    case ST_FULL:          return "FULL";
    case ST_ERROR:         return "ERROR";
    case ST_FAULT_LATCHED: return "FAULT_LATCHED";
    case ST_SLEEP:         return "SLEEP";
    case ST_MAINTENANCE:   return "MAINT";
  }
  return "?";
}

bool sm_isPumpRunningState(SystemState s) {
  return s == ST_AUTO_FILLING || s == ST_MANUAL_ON || s == ST_TIMER_RUNNING;
}

SystemState sm_state() { return s_state; }

uint32_t sm_pumpStartedAt() { return s_pumpStartedAt; }

// ---- One-shot overflow override -----------------------------------------
bool sm_overflowIgnore() { return s_overflowIgnore; }

void sm_setOverflowIgnore(bool on) {
  s_overflowIgnore  = on;
  s_overflowArmedAt = millis();
  Serial.printf("%s overflow-ignore %s\n", LOG_TAG_SM, on ? "ARMED (1 run)" : "cleared");
}

bool sm_toggleOverflowIgnore() {
  sm_setOverflowIgnore(!s_overflowIgnore);
  return s_overflowIgnore;
}

// True only when the active/pending run is a MANUAL or TIMER run AND the
// one-shot override is armed. AUTO fills always respect the 100% cutoff.
static bool overflowAllowedNow() {
  if (!s_overflowIgnore) return false;
  if (s_state == ST_AUTO_FILLING) return false;
  // During STARTING, infer the intended run type (AUTO if not a timer and mode==AUTO).
  if (s_state == ST_STARTING && !s_timerActive && settings().mode == MODE_AUTO) return false;
  return true;
}

// v6: a pump start is only safe when the tank node's telemetry is live —
// without it we have neither level (overflow protection) nor flow (dry-run
// detection). Refuse all starts (AUTO, MANUAL, TIMER) while the link is down.
static bool canStartPump() {
  if (!link_alive()) {
    Serial.printf("%s start refused — link down (no level/flow)\n", LOG_TAG_SM);
    return false;
  }
  return true;
}

void sm_clearLatched() {
  if (s_state == ST_FAULT_LATCHED || s_state == ST_ERROR) {
    s_state = ST_IDLE; s_enteredAt = millis();
    Serial.printf("%s cleared → IDLE\n", LOG_TAG_SM);
  }
}

static void enterState(SystemState ns) {
  if (ns == s_state) return;
  s_prev = s_state;
  s_state = ns;
  s_enteredAt = millis();
  s_phaseStart = millis();
  s_sawFb = s_sawCurrent = s_sawFlow = false;
  Serial.printf("%s %s -> %s\n", LOG_TAG_SM, sm_stateName(s_prev), sm_stateName(s_state));
  ui_requestUpdate();  // trigger UI refresh on state change

  switch (ns) {
    case ST_STARTING:
      actuator_pulseOn();
      s_pumpStartedAt = millis();
      break;
    case ST_STOPPING:
      actuator_pulseOff();
      s_timerActive = false;
      s_overflowIgnore = false;   // one-shot override consumed on any stop
      break;
    case ST_FAULT_LATCHED:
      actuator_panicOff();
      break;
    default: break;
  }
}

static void recordFault(FaultCode c, FaultSeverity sev) {
  FaultEntry fe{};
  fe.ts = millis();
  fe.state = (uint8_t)s_state;
  fe.code = c; fe.severity = sev;
  fe.levelPct = sensors_levelPct();
  fe.flowLpmX10 = sensors_flowLpmX10();
  fe.currentMv  = sensors_currentMv();
  faultlog_record(fe);

  // Only escalate for ERROR or PANIC, never for INFO/WARN.
  if (sev < SEV_ERROR) return;

  // Only escalate to FAULT_LATCHED for repeated ERROR or any PANIC.
  if (c == s_rep.last && (uint32_t)(millis() - s_rep.firstAt) < DEF_FAULT_REPEAT_WINDOW) {
    s_rep.count++;
  } else {
    s_rep.last = c; s_rep.firstAt = millis(); s_rep.count = 1;
  }
  if (s_rep.count >= DEF_FAULT_REPEAT_LIMIT || sev == SEV_PANIC) {
    enterState(ST_FAULT_LATCHED);
    s_rep.count = 0;
  } else {
    enterState(ST_ERROR);
  }
}

// ============== event handling ==============
void sm_handleEvent(const Event& e) {
  switch (e.type) {

    case EV_PANIC_STOP:
      actuator_panicOff();
      enterState(ST_FAULT_LATCHED);
      return;

    case EV_FAULT:
      recordFault(e.p.fault.code, e.p.fault.severity);
      return;

    // ---- v6 wireless link ----
    case EV_LINK_DOWN:
      // Lost the tank node's sensor telemetry. Without floats/flow we are
      // blind, so fail safe: stop any active pump and record the fault.
      // AUTO restarts are blocked in sm_tick() while the link is down.
      Serial.printf("%s LINK DOWN (age=%lums) — failing safe\n",
        LOG_TAG_SM, (unsigned long)e.p.u32);
      if (sm_isPumpRunningState(s_state) || s_state == ST_STARTING) {
        recordFault(FC_LINK_LOST, SEV_ERROR);
        enterState(ST_STOPPING);
      } else {
        // Log as a warning so it appears in the fault history even when idle.
        FaultEntry fe{}; fe.ts = millis(); fe.state = (uint8_t)s_state;
        fe.code = FC_LINK_LOST; fe.severity = SEV_WARN;
        fe.levelPct = sensors_levelPct();
        faultlog_record(fe);
      }
      // No popup: the dashboard link icon already shows this continuously.
      ui_requestUpdate();
      return;

    case EV_LINK_UP:
      Serial.printf("%s LINK UP (seq=%lu)\n", LOG_TAG_SM, (unsigned long)e.p.u32);
      ui_requestUpdate();
      return;

    case EV_SELFTEST_RESULT:
      if (e.p.u32 == 0) {
        // restore mode (but never auto-resume MANUAL/TIMER)
        uint8_t m = settings().mode;
        if (settings().sleepMode)               enterState(ST_SLEEP);
        else if (m == MODE_MAINTENANCE)         enterState(ST_MAINTENANCE);
        else                                    enterState(ST_IDLE);
      } else {
        // Surface which check failed — bit 2=CT bias, 3=feedback stuck, 4=NVS.
        char buf[24];
        snprintf(buf, sizeof(buf), "SELFTEST 0x%02X", (unsigned)e.p.u32);
        ui_showPopup(buf, 6000);
        enterState(ST_MAINTENANCE);
      }
      return;

    case EV_MODE_REQ: {
      uint8_t m = (uint8_t)e.p.i32;
      // Refuse manual when in SLEEP
      if (s_state == ST_SLEEP && m == MODE_MANUAL) return;
      settings_setU8("mode", m);
      if (m == MODE_SLEEP) { settings_setBool("sleepMode", true); enterState(ST_SLEEP); }
      else if (m == MODE_MAINTENANCE) enterState(ST_MAINTENANCE);
      else if (m == MODE_AUTO) {
        if (s_state == ST_SLEEP || s_state == ST_MAINTENANCE) {
          settings_setBool("sleepMode", false);
          enterState(ST_IDLE);
        }
      }
      return;
    }

    case EV_BUTTON: {
      Serial.printf("%s BTN%u %s\n", LOG_TAG_SM,
        (unsigned)e.p.btn.id, e.p.btn.kind == PRESS_LONG ? "LONG" : "SHORT");

      // Any key wakes the panel; B1 is the dedicated wake key, so its first
      // press is consumed rather than also silencing / switching screens.
      {
        bool wasDim = ui_isDim();
        ui_wake();
        if (wasDim && e.p.btn.id == BTN1) return;
      }

      // If menu is open (or about to open via B4 short), let menu consume input.
      if (menu_isOpen() || (e.p.btn.id == BTN4 && e.p.btn.kind == PRESS_SHORT)) {
        menu_handleButton(e.p.btn.id, e.p.btn.kind);
        return;
      }

      // Operating screen toggle: B1 short while pump running toggles home/op view
      if (e.p.btn.id == BTN1 && e.p.btn.kind == PRESS_SHORT && ui_isOpScreen()) {
        ui_toggleOpScreen();
        return;
      }
      // Also allow toggling back to op screen from home while running
      if (e.p.btn.id == BTN1 && e.p.btn.kind == PRESS_SHORT &&
          (sm_isPumpRunningState(s_state) || s_state == ST_STARTING) && !ui_isOpScreen()) {
        ui_toggleOpScreen();
        return;
      }

      if (e.p.btn.id == BTN3 && e.p.btn.kind == PRESS_LONG) {
        // HARD STOP / reset
        if (sm_isPumpRunningState(s_state) || s_state == ST_STARTING) {
          enterState(ST_STOPPING);
        } else if (s_state == ST_FAULT_LATCHED || s_state == ST_ERROR) {
          sm_clearLatched();
        }
      } else if (e.p.btn.id == BTN4 && e.p.btn.kind == PRESS_LONG) {
        // toggle SLEEP
        bool now = !settings().sleepMode;
        settings_setBool("sleepMode", now);
        enterState(now ? ST_SLEEP : ST_IDLE);
      } else if (e.p.btn.id == BTN1 && e.p.btn.kind == PRESS_SHORT) {
        // silence buzzer (only when pump NOT running — running case handled above).
        // Acknowledges the FULL/ERROR/LATCHED alert until the state changes.
        buzzer_silence();
        Serial.printf("%s buzzer silence\n", LOG_TAG_SM);
      } else if (e.p.btn.id == BTN1 && e.p.btn.kind == PRESS_LONG) {
        // toggle AUTO/MANUAL
        if (s_state == ST_SLEEP || s_state == ST_MAINTENANCE) return;
        if (s_state == ST_MANUAL_ON) {
          settings_setU8("mode", MODE_AUTO);
          enterState(ST_STOPPING);
        } else if (s_state == ST_IDLE || (s_state == ST_FULL && s_overflowIgnore)) {
          // MANUAL start; from FULL only when the overflow override is armed.
          settings_setU8("mode", MODE_MANUAL);
          // Clear sleep flag so IDLE tick doesn't push us back to SLEEP
          if (settings().sleepMode) settings_setBool("sleepMode", false);
#if !MONITOR_ONLY_MODE || ALLOW_MANUAL_ACTUATION
          if (canStartPump()) enterState(ST_STARTING);
#else
          Serial.printf("%s MANUAL request ignored (monitor-only)\n", LOG_TAG_SM);
#endif
        }
      } else if (e.p.btn.id == BTN2 && e.p.btn.kind == PRESS_LONG) {
        // Arm/disarm the one-shot overflow override (deliberate run past full).
        // Pairs with B2 (the timed-run button): hold to arm, tap to run.
        bool on = sm_toggleOverflowIgnore();
        ui_showPopup(on ? "OVERFLOW ARM 1x" : "OVERFLOW OFF", 2500);
        buzzer_chirp(on ? 300 : 100);
        ui_requestUpdate();
      } else if (e.p.btn.id == BTN2 && e.p.btn.kind == PRESS_SHORT) {
        // TIMER1 start; from FULL only when the overflow override is armed.
        if (s_state != ST_IDLE && !(s_state == ST_FULL && s_overflowIgnore)) return;
        if (settings().sleepMode) settings_setBool("sleepMode", false);
#if !MONITOR_ONLY_MODE || ALLOW_MANUAL_ACTUATION
        if (canStartPump()) {
          s_timerActive = true;
          s_timerWhich = TMR_1;
          s_timerDeadline = millis() + settings().timer1Ms;
          enterState(ST_STARTING);
        }
#else
        Serial.printf("%s TIMER1 ignored (monitor-only)\n", LOG_TAG_SM);
#endif
      } else if (e.p.btn.id == BTN3 && e.p.btn.kind == PRESS_SHORT) {
        // TIMER2 start; from FULL only when the overflow override is armed.
        if (s_state != ST_IDLE && !(s_state == ST_FULL && s_overflowIgnore)) return;
        if (settings().sleepMode) settings_setBool("sleepMode", false);
#if !MONITOR_ONLY_MODE || ALLOW_MANUAL_ACTUATION
        if (canStartPump()) {
          s_timerActive = true;
          s_timerWhich = TMR_2;
          s_timerDeadline = millis() + settings().timer2Ms;
          enterState(ST_STARTING);
        }
#else
        Serial.printf("%s TIMER2 ignored (monitor-only)\n", LOG_TAG_SM);
#endif
      }
      return;
    }

    case EV_LEVEL_CHANGED:
      // Reaching 100% is a normal stop, not a fault. Only transition states.
      if (e.p.i32 >= 100) {
        if (sm_isPumpRunningState(s_state) || s_state == ST_STARTING) {
          // MANUAL/TIMER may deliberately run past full when the one-shot
          // overflow override is armed; AUTO always caps at 100%.
          if (!overflowAllowedNow()) enterState(ST_STOPPING);
        } else if (s_state == ST_IDLE) {
          enterState(ST_FULL);
        }
        // Do NOT record overflow fault here; only escalate if safety supervisor detects true error.
      }
      return;

    case EV_FB_ON:
      if (s_state == ST_STARTING && e.p.boolean) s_sawFb = true;
      // Smart-Sense: feedback switch pressed externally while IDLE
      if (s_state == ST_IDLE && e.p.boolean && settings().smartSense && !settings().bypassFeedback) {
        Serial.printf("%s SMART-SENSE: feedback detected, entering monitoring\n", LOG_TAG_SM);
        enterState(ST_MANUAL_ON);  // enter running state for monitoring
        s_pumpStartedAt = millis(); // anchor runtime/max-runtime to NOW (not boot)
        ui_requestUpdate();
      }
      return;

    case EV_FB_OFF:
      if (s_state == ST_STOPPING && e.p.boolean) s_sawFb = true;
      return;

    case EV_CURRENT_PRESENT:
      if (s_state == ST_STARTING && e.p.boolean) s_sawCurrent = true;
      else if (s_state == ST_STOPPING && !e.p.boolean) s_sawCurrent = true;
      else if (sm_isPumpRunningState(s_state) && !e.p.boolean) {
        // Only fault if current sense is NOT bypassed
        if (!settings().bypassCurrentSense) {
          recordFault(FC_NO_CURRENT, SEV_ERROR);
          enterState(ST_STOPPING);
        }
      }
      // Smart-Sense: current detected externally while IDLE
      else if (s_state == ST_IDLE && e.p.boolean && settings().smartSense && !settings().bypassCurrentSense) {
        Serial.printf("%s SMART-SENSE: current detected, entering monitoring\n", LOG_TAG_SM);
        enterState(ST_MANUAL_ON);
        s_pumpStartedAt = millis();
        ui_requestUpdate();
      }
      return;

    case EV_FLOW_TICK:
      if (s_state == ST_STARTING && e.p.flow.lpm_x10 >= sensors_flowThreshX10()) s_sawFlow = true;
      else if (s_state == ST_STOPPING && e.p.flow.lpm_x10 < sensors_flowThreshX10()) s_sawFlow = true;
      // Smart-Sense: flow detected externally while IDLE
      else if (s_state == ST_IDLE && e.p.flow.lpm_x10 >= sensors_flowThreshX10()
               && settings().smartSense && !settings().bypassFlowSense) {
        Serial.printf("%s SMART-SENSE: flow detected (%u x0.1 lpm), entering monitoring\n",
          LOG_TAG_SM, e.p.flow.lpm_x10);
        enterState(ST_MANUAL_ON);
        s_pumpStartedAt = millis();
        ui_requestUpdate();
      }
      return;

    case EV_TIMER_EXPIRED:
      if (e.p.i32 == TMR_MAX_RUNTIME) {
        recordFault(FC_OVERRUN, SEV_ERROR);
        enterState(ST_STOPPING);
      } else if (s_timerActive) {
        enterState(ST_STOPPING);
      }
      return;

    default: return;
  }
}

// ============== periodic tick ==============
void sm_tick() {

  // One-shot overflow override auto-disarms if armed but no run has started
  // within the timeout — prevents a stale "armed earlier" surprise overflow.
  // Only counts down while NOT running/starting; once a run begins the flag
  // persists until the run stops (consumed on STOPPING).
  if (s_overflowIgnore && !sm_isPumpRunningState(s_state) && s_state != ST_STARTING
      && sinceMs(s_overflowArmedAt) > OVERFLOW_ARM_TIMEOUT_MS) {
    s_overflowIgnore = false;
    ui_showPopup("OVERFLOW EXPIRED", 2000);
    buzzer_chirp(100);
    Serial.printf("%s overflow-ignore auto-disarmed (unused)\n", LOG_TAG_SM);
  }

  switch (s_state) {

    case ST_BOOT_SELFTEST:
      // wait for SELFTEST_RESULT event
      break;

    case ST_IDLE: {
      if (settings().sleepMode) { enterState(ST_SLEEP); break; }
#if !MONITOR_ONLY_MODE
      // v6: only auto-fill when the tank node link is live (canStartPump()).
      if (settings().mode == MODE_AUTO && link_alive() && sensors_levelPct() < 25) {
        enterState(ST_STARTING);
      }
#endif
      break;
    }

    case ST_FULL:
      if (sensors_levelPct() < (100 - settings().levelHystPct)) enterState(ST_IDLE);
      break;

    case ST_SLEEP:
      if (!settings().sleepMode) enterState(ST_IDLE);
      break;

    case ST_STARTING: {
      uint32_t since = sinceMs(s_phaseStart);
      // If bypass flags set, consider those checks as "passed"
      bool needFb      = !settings().bypassFeedback;
      bool needCurrent = !settings().bypassCurrentSense;
      bool needFlow    = !settings().bypassFlowSense;
      bool gotFb      = needFb      ? s_sawFb      : true;
      bool gotCurrent = needCurrent ? s_sawCurrent : true;
      bool gotFlow    = needFlow    ? s_sawFlow    : true;

      if (gotFb && gotCurrent && gotFlow) {
        // Decide which running state
        if (s_timerActive) enterState(ST_TIMER_RUNNING);
        else if (settings().mode == MODE_MANUAL) enterState(ST_MANUAL_ON);
        else enterState(ST_AUTO_FILLING);
        break;
      }
      if (needFb && !s_sawFb && since > settings().feedbackMs) {
        recordFault(FC_NO_FEEDBACK, SEV_ERROR);
        enterState(ST_STOPPING);
        break;
      }
      if (needCurrent && !s_sawCurrent && since > settings().currentMs) {
        recordFault(FC_NO_CURRENT, SEV_ERROR);
        enterState(ST_STOPPING);
        break;
      }
      if (needFlow && !s_sawFlow && since > settings().dryRunMs) {
        recordFault(FC_DRY_RUN, SEV_ERROR);
        enterState(ST_STOPPING);
        break;
      }
      break;
    }

    case ST_STOPPING: {
      uint32_t since = sinceMs(s_phaseStart);
      bool current = settings().bypassCurrentSense ? false : sensors_currentPresent();
      bool flow    = settings().bypassFlowSense    ? false : (sensors_flowLpmX10() >= sensors_flowThreshX10());
      bool gotFb   = settings().bypassFeedback     ? true  : s_sawFb;
      if (gotFb && !current && !flow) {
        // Decide where to go
        enterState(sensors_levelPct() >= 100 ? ST_FULL : ST_IDLE);
        break;
      }
      if (!settings().bypassFeedback && since > settings().feedbackMs && !s_sawFb) {
        recordFault(FC_NO_FEEDBACK, SEV_ERROR);
        // re-attempt one more pulse
        actuator_pulseOff();
        s_phaseStart = millis();
        break;
      }
      if (!settings().bypassCurrentSense && since > settings().currentOffMs && sensors_currentPresent()) {
        recordFault(FC_STUCK_PUMP, SEV_PANIC);
        enterState(ST_FAULT_LATCHED);
        break;
      }
      if (!settings().bypassFlowSense && since > settings().flowOffMs && sensors_flowLpmX10() >= sensors_flowThreshX10()) {
        recordFault(FC_STUCK_PUMP, SEV_PANIC);
        enterState(ST_FAULT_LATCHED);
        break;
      }
      break;
    }

    case ST_AUTO_FILLING:
    case ST_MANUAL_ON:
    case ST_TIMER_RUNNING: {
      // Reaching 100% is a normal stop, not a fault — go straight to STOPPING.
      // Exception: a MANUAL/TIMER run with the one-shot overflow override armed
      // keeps running past full ("get water even if the tank is full").
      if (sensors_levelPct() >= 100 && !overflowAllowedNow()) {
        enterState(ST_STOPPING);
        break;
      }
      // max runtime
      if (sinceMs(s_pumpStartedAt) > settings().maxRuntimeMs) {
        recordFault(FC_OVERRUN, SEV_ERROR);
        enterState(ST_STOPPING);
        break;
      }
      // timer
      if (s_state == ST_TIMER_RUNNING && s_timerActive && (int32_t)(millis() - s_timerDeadline) >= 0) {
        Event e{}; e.type = EV_TIMER_EXPIRED; e.p.i32 = s_timerWhich;
        sendEvent(e);
      }
      break;
    }

    case ST_ERROR:
      if (sinceMs(s_enteredAt) > DEF_OVERRUN_COOLDOWN_MS) enterState(ST_IDLE);
      break;

    case ST_FAULT_LATCHED:
    case ST_MAINTENANCE:
    default:
      break;
  }
}

void sm_init() {
  s_state = ST_BOOT_SELFTEST;
  s_enteredAt = millis();
}
