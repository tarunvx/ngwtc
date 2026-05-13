#include "buttons.h"
#include "pins.h"
#include "config.h"
#include "events.h"
#include "event_queue.h"
#include "time_utils.h"

// ============================================================
//  Button handling — debounce + short / long classification
//  All 4 pins use INPUT_PULLUP, active LOW.
// ============================================================

#define DEBOUNCE_MS    50      // sample stability window
#define HOLD_GUARD_MS  300     // ignore re-press right after release
#define LONG_PRESS_MS  1500

struct Btn {
  uint8_t  pin;
  ButtonId id;
  bool     stable;       // debounced state (true = pressed)
  bool     last;         // last raw read
  uint32_t lastChange;
  uint32_t pressedAt;
  uint32_t releasedAt;
  bool     longFired;
};

static Btn s_btns[4] = {
  { PIN_BTN_1, BTN1, false, false, 0, 0, 0, false },
  { PIN_BTN_2, BTN2, false, false, 0, 0, 0, false },
  { PIN_BTN_3, BTN3, false, false, 0, 0, 0, false },
  { PIN_BTN_4, BTN4, false, false, 0, 0, 0, false },
};

void buttons_init() {
  for (auto& b : s_btns) {
    pinMode(b.pin, INPUT_PULLUP);
  }
  Serial.printf("[BTN] init: B1=GPIO%u B2=GPIO%u B3=GPIO%u B4=GPIO%u\n",
    PIN_BTN_1, PIN_BTN_2, PIN_BTN_3, PIN_BTN_4);
}

static const char* kindName(PressKind k) {
  return k == PRESS_LONG ? "LONG" : "SHORT";
}

static void emit(ButtonId id, PressKind k) {
  Serial.printf("[BTN] B%u %s\n", (unsigned)id, kindName(k));
  Event e{};
  e.type = EV_BUTTON;
  e.p.btn = { id, k };
  sendEvent(e);
}

void buttons_tick() {
  uint32_t now = millis();
  for (auto& b : s_btns) {
    bool raw = (digitalRead(b.pin) == LOW);   // active LOW

    if (raw != b.last) {
      b.last = raw;
      b.lastChange = now;
    }

    if ((uint32_t)(now - b.lastChange) >= DEBOUNCE_MS && b.stable != b.last) {
      // hold guard: ignore press too soon after release
      if (b.last && b.releasedAt &&
          (uint32_t)(now - b.releasedAt) < HOLD_GUARD_MS) continue;

      b.stable = b.last;
      if (b.stable) {
        b.pressedAt = now;
        b.longFired = false;
      } else {
        b.releasedAt = now;
        if (!b.longFired) emit(b.id, PRESS_SHORT);
      }
    }

    if (b.stable && !b.longFired &&
        (uint32_t)(now - b.pressedAt) >= LONG_PRESS_MS) {
      b.longFired = true;
      emit(b.id, PRESS_LONG);
    }
  }
}