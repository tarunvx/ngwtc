#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <Arduino.h>

// Wrap-safe elapsed check (millis() rolls over every ~49 days).
static inline bool elapsed(uint32_t since, uint32_t window) {
  return (uint32_t)(millis() - since) >= window;
}

static inline uint32_t sinceMs(uint32_t t0) {
  return (uint32_t)(millis() - t0);
}

#endif
