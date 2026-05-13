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

#endif
