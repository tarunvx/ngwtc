#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "events.h"

enum SystemState : uint8_t {
  ST_BOOT_SELFTEST = 0,
  ST_IDLE,
  ST_AUTO_FILLING,
  ST_MANUAL_ON,
  ST_TIMER_RUNNING,
  ST_STARTING,
  ST_STOPPING,
  ST_FULL,
  ST_ERROR,
  ST_FAULT_LATCHED,
  ST_SLEEP,
  ST_MAINTENANCE,
};

void          sm_init();
void          sm_handleEvent(const Event& e);
void          sm_tick();        // periodic — timeouts, transitions
SystemState   sm_state();
const char*   sm_stateName(SystemState s);
bool          sm_isPumpRunningState(SystemState s);
void          sm_clearLatched();
uint32_t      sm_pumpStartedAt(); // millis() when pump last entered STARTING
bool          sm_fullAlarmActive();  // MD1 tank-full alarm is sounding

// One-shot "ignore full-tank cutoff" override (deliberate overflow run).
// Volatile / RAM-only — NEVER persisted, defaults false every boot. Consumed
// (auto-cleared) when the pump next stops, and auto-disarms if armed but no run
// is started within the arm timeout. Only affects MANUAL/TIMER runs; AUTO
// always respects the 100% cutoff.
bool          sm_overflowIgnore();          // is the override currently armed?
void          sm_setOverflowIgnore(bool on);
bool          sm_toggleOverflowIgnore();    // returns new state (for B2-long / menu)

#endif
