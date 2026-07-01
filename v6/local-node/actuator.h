#ifndef ACTUATOR_H
#define ACTUATOR_H

#include <Arduino.h>

void actuator_init();

// Pulse helpers — non-blocking; both relay pins guaranteed not high together.
void actuator_pulseOn();      // pulse ON-solenoid for settings.pulseOnMs
void actuator_pulseOff();     // pulse OFF-solenoid for settings.pulseOffMs

// Periodic — call from control task to clear pulses on time.
void actuator_tick();

// Panic — both relay pins LOW immediately + queue OFF pulse.
void actuator_panicOff();

bool actuator_isPulsing();

#endif
