#include "led.h"
#include "config.h"
#include "pins.h"
#include "sensors.h"
#include "settings.h"
#include "state_machine.h"
#include "time_utils.h"
#include "buzzer.h"

#if HAS_NEOPIXEL
  #include <Adafruit_NeoPixel.h>
  static Adafruit_NeoPixel strip(NEO_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
#endif

void led_init() {
#if HAS_NEOPIXEL
  strip.begin();
  strip.setBrightness(40);
  strip.clear();
  strip.show();

  // Startup rainbow chase animation (~1.5s)
  // Each LED lights one-by-one in rainbow colors, then all fade out
  const uint8_t hueStep = 256 / NEO_COUNT;  // spread rainbow across strip
  for (uint8_t i = 0; i < NEO_COUNT; i++) {
    strip.clear();
    // Light current LED + trail of 2 behind it
    for (int8_t t = 0; t < 3 && (i - t) >= 0; t++) {
      uint8_t idx = i - t;
      uint8_t bright = 255 >> t;  // 255, 127, 63
      uint16_t hue = (uint16_t)(idx * hueStep) * 256;  // map to 0-65535
      strip.setPixelColor(idx, strip.ColorHSV(hue, 255, bright));
    }
    strip.show();
    // Startup chime: three short beeps rising with the chase. Boot-only — the
    // buzzer task isn't running yet, so buzzer_beep() drives the pin directly.
    if (i == 1 || i == 4 || i == 7) { buzzer_beep(60); delay(40); }
    else delay(100);  // ~100 ms per LED so the chase still takes ~1 s
  }
  // Hold full rainbow briefly
  for (uint8_t i = 0; i < NEO_COUNT; i++) {
    uint16_t hue = (uint16_t)(i * hueStep) * 256;
    strip.setPixelColor(i, strip.ColorHSV(hue, 255, 200));
  }
  strip.show();
  delay(400);
  // Fade out
  for (uint8_t b = 200; b > 0; b -= 20) {
    for (uint8_t i = 0; i < NEO_COUNT; i++) {
      uint16_t hue = (uint16_t)(i * hueStep) * 256;
      strip.setPixelColor(i, strip.ColorHSV(hue, 255, b));
    }
    strip.show();
    delay(20);
  }
  strip.clear();
  strip.show();
#endif
}

void led_tick() {
#if HAS_NEOPIXEL
  static uint32_t last = 0;
  if (!elapsed(last, 100)) return;
  last = millis();

  SystemState st = sm_state();
  uint8_t lvl = sensors_levelPct();
  strip.clear();

  // Fault and special states override normal color mapping
  if (st == ST_FAULT_LATCHED) {
    uint32_t c = (millis()/200)%2 ? strip.Color(255,0,0) : 0;
    for (uint8_t i = 0; i < NEO_COUNT; i++) strip.setPixelColor(i, c);
    strip.show();
    return;
  }
  if (st == ST_ERROR) {
    uint32_t c = (millis()/600)%2 ? strip.Color(255,0,0) : 0;
    for (uint8_t i = 0; i < NEO_COUNT; i++) strip.setPixelColor(i, c);
    strip.show();
    return;
  }
  if (st == ST_SLEEP) {
    strip.setPixelColor(NEO_COUNT-1, strip.Color(0,30,40));
    strip.show();
    return;
  }
  if (st == ST_MAINTENANCE) {
    for (uint8_t i = 0; i < NEO_COUNT; i++) strip.setPixelColor(i, strip.Color(255,80,0));
    strip.show();
    return;
  }
  if (st == ST_FULL) {
    for (uint8_t i = 0; i < NEO_COUNT; i++) strip.setPixelColor(i, strip.Color(0,50,0));
    strip.show();
    return;
  }

  // color mapping for water level (top-down fill — strip mounted inverted)
  // Index 0 = physical TOP, index NEO_COUNT-1 = physical BOTTOM
  // Level fills from top downward:
  // 0%   = top 2 LEDs red blinking (empty warning)
  // <25% = top 2 LEDs red solid
  // 25%  = top 3 LEDs orange
  // 50%  = top 5 LEDs yellow
  // 75%  = top 7 LEDs blue
  // 100% = all 9 LEDs green
  if (lvl == 0) {
    // Empty — blink red for attention (top 2)
    uint32_t c = (millis()/500)%2 ? strip.Color(255,0,0) : 0;
    strip.setPixelColor(NEO_COUNT-1, c);
    strip.setPixelColor(NEO_COUNT-2, c);
  } else if (lvl < 25) {
    strip.setPixelColor(NEO_COUNT-1, strip.Color(255,0,0));
    strip.setPixelColor(NEO_COUNT-2, strip.Color(255,0,0));
  } else if (lvl < 50) {
    for (uint8_t i = 0; i < 3; i++)
      strip.setPixelColor(NEO_COUNT-1-i, strip.Color(180,80,0));  // orange
  } else if (lvl < 75) {
    for (uint8_t i = 0; i < 5; i++)
      strip.setPixelColor(NEO_COUNT-1-i, strip.Color(180,180,0)); // yellow
  } else if (lvl < 100) {
    for (uint8_t i = 0; i < 7; i++)
      strip.setPixelColor(NEO_COUNT-1-i, strip.Color(0,0,120));   // blue
  } else {
    for (uint8_t i = 0; i < NEO_COUNT; i++)
      strip.setPixelColor(i, strip.Color(0,80,0));    // green
  }
  strip.show();
#endif
}
