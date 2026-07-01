#include "safety.h"
#include "state_machine.h"
#include "actuator.h"
#include "sensors.h"
#include "events.h"
#include "event_queue.h"
#include "settings.h"
#include "config.h"
#include "time_utils.h"

void safety_init() {}

void safety_tick() {
  SystemState st = sm_state();

  // Independent overrun watchdog (defense-in-depth alongside SM)
  static uint32_t s_runStart = 0;
  static SystemState s_lastSt = ST_IDLE;
  if (sm_isPumpRunningState(st)) {
    if (!sm_isPumpRunningState(s_lastSt)) s_runStart = millis();
    if (sinceMs(s_runStart) > settings().maxRuntimeMs + 5000UL) {
      // SM should have caught it — escalate
      actuator_panicOff();
      Event e{}; e.type = EV_PANIC_STOP; e.p.i32 = FC_OVERRUN;
      sendEvent(e);
    }
  }
  s_lastSt = st;

  // Note: 100% level during a running state is handled by the state machine
  // (transitions to STOPPING). We do NOT re-emit OVERFLOW faults here, to
  // avoid escalating a normal full-tank condition.

  // Stuck pump detection — pump should be OFF but current/flow continues
  if (st == ST_IDLE || st == ST_FULL || st == ST_SLEEP) {
    if (sensors_currentPresent() && sensors_flowLpmX10() >= sensors_flowThreshX10()) {
      Event e{}; e.type = EV_PANIC_STOP; e.p.i32 = FC_STUCK_PUMP;
      sendEvent(e);
    }
  }
}
