#include "buzzer.h"
#include "pins.h"
#include "time_utils.h"
#include "state_machine.h"
#include "settings.h"
#include <Arduino.h>

static BuzzPattern s_pat = BZ_NONE;
static uint32_t    s_t0  = 0;
static bool        s_silenced = false;
static uint32_t    s_chirpUntil = 0;   // one-shot confirmation chirp deadline

// Buzzer hardware: a PASSIVE buzzer (no internal oscillator) on G23. It needs an
// AC/square-wave drive to make sound, so we use the ESP32 LEDC peripheral: a
// 2 kHz carrier at 50% duty (128/255) = a clean tone; duty 0 = silent. (Tested
// good on this module.) Pattern durations/duty in buzzer_tick() are unchanged —
// each "on" just switches the LEDC duty to the tone instead of a bare HIGH.
#define BUZZER_FREQ_HZ  2000   // tone carrier frequency
#define BUZZER_RES_BITS 8      // LEDC resolution (duty 0..255)
#define BUZZER_DUTY_ON  128    // 50% square wave = clean tone
#define BUZZER_LEDC_CH  0      // (Arduino-ESP32 core 2.x channel API only)

// Low-level LEDC duty write (0 = silent, 128 = tone). Auto-selects the core-3.x
// pin API or the core-2.x channel API.
static void buzzWrite(uint16_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BUZZER, duty);
#else
  ledcWrite(BUZZER_LEDC_CH, duty);
#endif
}

void buzzer_init() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, BUZZER_FREQ_HZ, BUZZER_RES_BITS);
#else
  ledcSetup(BUZZER_LEDC_CH, BUZZER_FREQ_HZ, BUZZER_RES_BITS);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
#endif
  buzzWrite(0);   // silent at boot
}

void buzzer_set(BuzzPattern p) {
  if (p != s_pat) { s_pat = p; s_t0 = millis(); s_silenced = false; }
}

void buzzer_silence() { s_silenced = true; buzzWrite(0); }

// One-shot confirmation blip. Overrides the pattern engine (and a silence) for
// `ms`, then normal pattern output resumes. Used for button-ack feedback.
void buzzer_chirp(uint16_t ms) { s_chirpUntil = millis() + ms; }

// Blocking beep — ONLY for boot/setup, before the buzzer task starts (e.g. the
// startup chime played during the LED rainbow). Drives the pin directly.
void buzzer_beep(uint16_t onMs) {
  buzzWrite(BUZZER_DUTY_ON);
  delay(onMs);
  buzzWrite(0);
}

static void out(bool on) { buzzWrite((on && !s_silenced) ? BUZZER_DUTY_ON : 0); }

void buzzer_tick() {
  // One-shot confirmation chirp takes priority over the pattern engine (and a
  // silence) for its brief duration — used for button-ack feedback.
  if ((int32_t)(s_chirpUntil - millis()) > 0) { buzzWrite(BUZZER_DUTY_ON); return; }
  uint32_t t = sinceMs(s_t0);
  switch (s_pat) {
    case BZ_NONE:     out(false); break;
    case BZ_SHORT:    out(t < 100); if (t > 100) s_pat = BZ_NONE; break;
    case BZ_RUN: {
      // Pump running: a "still alive" signature once a minute rather than a
      // constant heartbeat, which was tiring to listen to. One long pulse, a
      // gap, then one short pulse.
      uint32_t p = t % 60000;
      out(p < 400 || (p >= 600 && p < 720));
      break;
    }
    case BZ_FULL:
      // Tank full: a steady, insistent beep (~1.7 Hz, 300 ms on / 300 ms off)
      // until acknowledged (B1 silence) or the level drops out of FULL — the
      // "come turn it off" alert. A repeating beep (not constant DC) so it is
      // audible on both active buzzer modules AND passive piezos (a passive
      // piezo makes no sound on a steady DC level — only on edges).
      out((t % 600) < 300);
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

  // MD1 raises the tank-full alarm while the pump is still running, so it is
  // checked before the state map.
  if (sm_fullAlarmActive()) {
    want = BZ_FULL;
  } else {
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
  }

  // Global mute: everything is suppressed except a latched fault, which needs a
  // human to intervene and so must always be audible.
  if (settings().silentMode && want != BZ_LATCHED) want = BZ_NONE;

  if (want != s_pat) buzzer_set(want);
}
