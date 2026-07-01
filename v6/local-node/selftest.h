#ifndef SELFTEST_H
#define SELFTEST_H
#include <Arduino.h>
// Returns bitmask of failures (0 = all pass).
uint32_t selftest_run();
#endif
