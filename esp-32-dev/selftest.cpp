#include "selftest.h"
#include "pins.h"
#include "config.h"
#include "settings.h"

// Failure bits
#define ST_FAIL_FLOAT_PLAUS  (1U << 0)
#define ST_FAIL_FLOW_PIN     (1U << 1)
#define ST_FAIL_CT_BIAS      (1U << 2)
#define ST_FAIL_FB_STUCK     (1U << 3)
#define ST_FAIL_NVS          (1U << 4)

uint32_t selftest_run() {
  uint32_t fail = 0;

  // Float plausibility
  bool l25  = digitalRead(PIN_FLOAT_25)  == LOW;
  bool l50  = digitalRead(PIN_FLOAT_50)  == LOW;
  bool l75  = digitalRead(PIN_FLOAT_75)  == LOW;
  bool l100 = digitalRead(PIN_FLOAT_100) == LOW;
  if ((l100 && (!l75 || !l50 || !l25)) ||
      (l75  && (!l50 || !l25)) ||
      (l50  && !l25)) fail |= ST_FAIL_FLOAT_PLAUS;

  // Flow pin should idle HIGH (pull-up)
  if (digitalRead(PIN_FLOW) != HIGH) fail |= ST_FAIL_FLOW_PIN;

  // CT bias near mid-rail
#if HAS_CT_CLAMP
  // Average many reads: the CT rides on an AC signal and a single ADC sample is
  // noisy. Reported only — sampleCtRmsMv() auto-zeroes, so a bias that sits off
  // ctOffsetMv no longer affects the measurement and must not block boot.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 64; i++) sum += analogReadMilliVolts(PIN_CT_ADC);
  uint32_t mv = sum / 64;
  bool biasOdd = (mv + 400 < (uint32_t)settings().ctOffsetMv ||
                  mv > (uint32_t)settings().ctOffsetMv + 400);
  Serial.printf("[SELFTEST] CT bias %lumv (expect %u +/-400)%s\n",
    (unsigned long)mv, settings().ctOffsetMv, biasOdd ? " <-- CHECK WIRING" : "");
#endif

  // Feedback switches should be released at boot (pump off)
  if (digitalRead(PIN_FB_ON)  == LOW && digitalRead(PIN_FB_OFF) == LOW) fail |= ST_FAIL_FB_STUCK;

  // NVS already validated in settings_init; if magic mismatched, settings would have rewritten.
  if (settings().magic != 0x5A11) fail |= ST_FAIL_NVS;

  Serial.printf("[SELFTEST] result=0x%08X\n", fail);
  return fail;
}
