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

// ---- Transition trace ----------------------------------------------------
// Every state change records WHY it happened plus a snapshot of every input the
// decision could have depended on. Without this, an unexplained entry into a
// running state is undiagnosable after the fact.
enum TransitionReason : uint8_t {
  TR_UNKNOWN = 0, TR_BOOT, TR_SELFTEST_OK, TR_SELFTEST_FAIL,
  TR_BTN_MANUAL, TR_BTN_TIMER1, TR_BTN_TIMER2, TR_BTN_STOP, TR_BTN_SLEEP,
  TR_MQTT_ON, TR_MQTT_OFF, TR_MODE_REQ,
  TR_AUTO_LEVEL, TR_SMART_FB, TR_SMART_CUR, TR_SMART_FLOW,
  TR_MD1_FB_ON, TR_MD1_FB_OFF,
  TR_START_OK, TR_STOP_DONE, TR_LEVEL_FULL, TR_LEVEL_DROP,
  TR_MAXRUN, TR_TIMER_END, TR_FAULT, TR_PANIC, TR_LINK_DOWN,
  TR_CLEARED, TR_SLEEP, TR_WAKE,
  TR_COUNT
};

struct StateTrace {
  uint32_t ts;
  uint8_t  reason;
  uint8_t  from;
  uint8_t  to;
  uint8_t  mode;
  uint8_t  levelPct;
  uint8_t  flags;      // see SMTF_* below
  uint16_t flowX10;
  uint16_t currentMv;
  uint16_t ampsX10;
};

#define SMTF_FB_ON      0x01
#define SMTF_FB_OFF     0x02
#define SMTF_CUR_PRES   0x04
#define SMTF_LINK_OK    0x08
#define SMTF_BYP_I      0x10
#define SMTF_BYP_FLOW   0x20
#define SMTF_BYP_FB     0x40
#define SMTF_SMART      0x80

uint32_t    sm_traceSeq();      // RAM-only, monotonic since boot
size_t      sm_traceCount();
bool        sm_traceGet(size_t idx, StateTrace& out);
const char* sm_reasonName(uint8_t r);

// One-shot "ignore full-tank cutoff" override (deliberate overflow run).
// Volatile / RAM-only — NEVER persisted, defaults false every boot. Consumed
// (auto-cleared) when the pump next stops, and auto-disarms if armed but no run
// is started within the arm timeout. Only affects MANUAL/TIMER runs; AUTO
// always respects the 100% cutoff.
bool          sm_overflowIgnore();          // is the override currently armed?
void          sm_setOverflowIgnore(bool on);
bool          sm_toggleOverflowIgnore();    // returns new state (for B2-long / menu)

#endif
