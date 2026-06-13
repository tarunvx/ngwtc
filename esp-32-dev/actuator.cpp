#include "actuator.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include "events.h"
#include "event_queue.h"
#include "time_utils.h"

enum PulseState : uint8_t { PULSE_IDLE = 0, PULSE_ON_ACTIVE, PULSE_OFF_ACTIVE };

static volatile PulseState s_state = PULSE_IDLE;
static uint32_t s_pulseStart = 0;
static uint32_t s_pulseDur   = 0;

// Idle state: both pins HIGH (active-LOW relay modules — HIGH = coil off, no current draw)
static void allIdle() {
  digitalWrite(PIN_RELAY_ON,  HIGH);
  digitalWrite(PIN_RELAY_OFF, HIGH);
}

void actuator_init() {
  pinMode(PIN_RELAY_ON,  OUTPUT);
  pinMode(PIN_RELAY_OFF, OUTPUT);
  allIdle();
  // Defensive: re-issue OFF on every boot.
  actuator_pulseOff();
}

static void interlockCheck() {
  // Hardware/software invariant — if both ever read LOW (both relays energized), raise fault.
  if (digitalRead(PIN_RELAY_ON) == LOW && digitalRead(PIN_RELAY_OFF) == LOW) {
    allIdle();
    Event e{}; e.type = EV_FAULT;
    e.p.fault = { FC_INTERLOCK_VIOLATION, SEV_PANIC };
    sendEvent(e);
  }
}

void actuator_pulseOn() {
  if (s_state != PULSE_IDLE) return;            // never overlap
#if MONITOR_ONLY_MODE && !ALLOW_MANUAL_ACTUATION
  Serial.printf("%s pulseON SUPPRESSED (monitor-only)\n", LOG_TAG_ACT);
  return;
#endif
  digitalWrite(PIN_RELAY_OFF, HIGH);            // explicit interlock (de-energize OFF)
  digitalWrite(PIN_RELAY_ON,  LOW);             // energize ON relay
  s_pulseStart = millis();
  s_pulseDur   = settings().pulseOnMs;
  s_state      = PULSE_ON_ACTIVE;
  Serial.printf("%s pulseON %u ms\n", LOG_TAG_ACT, (unsigned)s_pulseDur);
}

void actuator_pulseOff() {
  if (s_state != PULSE_IDLE) return;
#if MONITOR_ONLY_MODE && !ALLOW_MANUAL_ACTUATION
  Serial.printf("%s pulseOFF SUPPRESSED (monitor-only)\n", LOG_TAG_ACT);
  return;
#endif
  digitalWrite(PIN_RELAY_ON,  HIGH);            // de-energize ON relay
  digitalWrite(PIN_RELAY_OFF, LOW);             // energize OFF relay
  s_pulseStart = millis();
  s_pulseDur   = settings().pulseOffMs;
  s_state      = PULSE_OFF_ACTIVE;
  Serial.printf("%s pulseOFF %u ms\n", LOG_TAG_ACT, (unsigned)s_pulseDur);
}

void actuator_tick() {
  if (s_state == PULSE_IDLE) return;
  interlockCheck();
  if (elapsed(s_pulseStart, s_pulseDur)) {
    allIdle();
    s_state = PULSE_IDLE;
  }
}

void actuator_panicOff() {
  allIdle();
  s_state = PULSE_IDLE;
  // re-issue OFF pulse to be sure
  actuator_pulseOff();
  Serial.printf("%s PANIC OFF\n", LOG_TAG_ACT);
}

bool actuator_isPulsing() { return s_state != PULSE_IDLE; }
