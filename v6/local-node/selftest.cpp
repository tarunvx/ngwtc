#include "selftest.h"
#include "pins.h"
#include "config.h"
#include "settings.h"
#include "link.h"

// Failure bits
#define ST_FAIL_FLOAT_PLAUS  (1U << 0)   // (v6: repurposed — reserved, no longer local)
#define ST_FAIL_FLOW_PIN     (1U << 1)   // (v6: repurposed — reserved, no longer local)
#define ST_FAIL_CT_BIAS      (1U << 2)
#define ST_FAIL_FB_STUCK     (1U << 3)
#define ST_FAIL_NVS          (1U << 4)

uint32_t selftest_run() {
  uint32_t fail = 0;

  // v6: floats + flow + pressure live on the TANK NODE and arrive over
  // ESP-NOW (see link.cpp), so there are no local pins to self-test here.
  // The link itself is NOT failed at boot — the tank node's first telemetry
  // frame may not have arrived yet; link liveness is supervised at runtime
  // (EV_LINK_DOWN). We only self-test hardware that is physically local.

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

  Serial.printf("[SELFTEST] result=0x%08X (v6: floats/flow remote)\n", fail);
  if (!link_everReceived()) {
    Serial.println(F("[SELFTEST] note: tank-node link not up yet (normal at boot)"));
  }
  return fail;
}
