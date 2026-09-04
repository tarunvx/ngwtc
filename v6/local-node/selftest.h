#ifndef SELFTEST_H
#define SELFTEST_H
#include <Arduino.h>

// Failure bits — the state machine decides which ones are severe enough to
// withhold control, so they must be visible outside selftest.cpp.
#define ST_FAIL_FLOAT_PLAUS  (1U << 0)   // v6: reserved, floats are remote
#define ST_FAIL_FLOW_PIN     (1U << 1)   // v6: reserved, flow is remote
#define ST_FAIL_CT_BIAS      (1U << 2)   // informational only
#define ST_FAIL_FB_STUCK     (1U << 3)
#define ST_FAIL_NVS          (1U << 4)

// Returns bitmask of failures (0 = all pass).
uint32_t selftest_run();
#endif
