#include "buzzer.h"
#include "pins.h"
#include "time_utils.h"
#include "state_machine.h"
#include <Arduino.h>

static BuzzPattern s_pat = BZ_NONE;
static uint32_t    s_t0  = 0;
static bool        s_silenced = false;
static uint32_t    s_chirpUntil = 0;   // one-shot confirmation chirp deadline

void buzzer_init() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}

void buzzer_set(BuzzPattern p) {
  if (p != s_pat) { s_pat = p; s_t0 = millis(); s_silenced = false; }
}

void buzzer_silence() { s_silenced = true; digitalWrite(PIN_BUZZER, LOW); }

// One-shot confirmation blip. Overrides the pattern engine (and a silence) for
// `ms`, then normal pattern output resumes. Used for button-ack feedback.
void buzzer_chirp(uint16_t ms) { s_chirpUntil = millis() + ms; }

static void out(bool on) { digitalWrite(PIN_BUZZER, (on && !s_silenced) ? HIGH : LOW); }

void buzzer_tick() {
  // One-shot confirmation chirp takes priority over the pattern engine (and a
  // silence) for its brief duration — used for button-ack feedback.
  if ((int32_t)(s_chirpUntil - millis()) > 0) { digitalWrite(PIN_BUZZER, HIGH); return; }
  uint32_t t = sinceMs(s_t0);
  switch (s_pat) {
    case BZ_NONE:     out(false); break;
    case BZ_SHORT:    out(t < 100); if (t > 100) s_pat = BZ_NONE; break;
    case BZ_RUN:
      // Pump running: a short, gentle chirp every 2 s ("I'm on") — runs
      // indefinitely until the state leaves a running state.
      out((t % 2000) < 80);
      break;
    case BZ_FULL:
      // Tank full: CONTINUOUS tone until acknowledged (B1 silence) or the
      // level drops out of FULL. This is the loud "come turn it off" alert.
      out(true);
      break;
    case BZ_ERROR:    out((t % 1000) < 100); break;
    case BZ_LATCHED:  out((t % 400)  < 200); break;
    case BZ_OVERFLOW: out((t % 250)  < 125); break;
  }
}

// Automatic policy: derive the desired buzzer pattern from the system state
// so callers don't have to sprinkle buzzer_set() everywhere. Called from the
// buzzer task each tick. buzzer_set() only re-arms (and clears silence) on an
// actual pattern change, so a user silence persists until the state changes.
void buzzer_autoTick() {
  BuzzPattern want;
  switch (sm_state()) {
    case ST_FULL:          want = BZ_FULL;    break;  // continuous alert
    case ST_FAULT_LATCHED: want = BZ_LATCHED; break;
    case ST_ERROR:         want = BZ_ERROR;   break;
    case ST_STARTING:
    case ST_AUTO_FILLING:
    case ST_MANUAL_ON:
    case ST_TIMER_RUNNING: want = BZ_RUN;     break;  // slow chirp while running
    default:               want = BZ_NONE;    break;
  }
  if (want != s_pat) buzzer_set(want);
}
