#include "buzzer.h"
#include "pins.h"
#include "time_utils.h"
#include <Arduino.h>

static BuzzPattern s_pat = BZ_NONE;
static uint32_t    s_t0  = 0;
static bool        s_silenced = false;

void buzzer_init() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}

void buzzer_set(BuzzPattern p) {
  if (p != s_pat) { s_pat = p; s_t0 = millis(); s_silenced = false; }
}

void buzzer_silence() { s_silenced = true; digitalWrite(PIN_BUZZER, LOW); }

static void out(bool on) { digitalWrite(PIN_BUZZER, (on && !s_silenced) ? HIGH : LOW); }

void buzzer_tick() {
  uint32_t t = sinceMs(s_t0);
  switch (s_pat) {
    case BZ_NONE:     out(false); break;
    case BZ_SHORT:    out(t < 100); if (t > 100) s_pat = BZ_NONE; break;
    case BZ_FULL: {
      // 200ms on / 800ms off, 3 cycles then stop
      uint32_t c = t / 1000;
      out((t % 1000) < 200 && c < 3);
      if (c >= 3) s_pat = BZ_NONE;
      break;
    }
    case BZ_ERROR:    out((t % 1000) < 100); break;
    case BZ_LATCHED:  out((t % 400)  < 200); break;
    case BZ_OVERFLOW: out((t % 250)  < 125); break;
  }
}
