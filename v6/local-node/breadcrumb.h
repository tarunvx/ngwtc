#ifndef BREADCRUMB_H
#define BREADCRUMB_H

#include <Arduino.h>

// Per-core "what was running" marker kept in RTC memory, which survives a
// watchdog or panic reset (but not a power cycle). INT_WDT freezes both cores
// at once, so on the next boot the core whose mark is OLDER is the one that
// was sitting in the section that held interrupts off.

enum BcPhase : uint8_t {
  BC_NONE = 0, BC_SETUP, BC_SELFTEST, BC_SENSORS, BC_LINK, BC_SM, BC_ACT,
  BC_SAFETY, BC_BUTTONS, BC_UI, BC_LED, BC_BUZZ, BC_MQTT, BC_FLUSH,
  BC_COUNT
};

// Call once, at the very top of setup(), before anything else marks.
void bc_captureBoot();

void bc_mark(uint8_t phase);

// Previous boot's marks, e.g. "c0:UI+318 c1:SENS+0" (bigger age = suspect).
const char* bc_report();

#endif
