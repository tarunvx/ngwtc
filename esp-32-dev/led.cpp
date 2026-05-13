#include "led.h"
#include "config.h"
#include "pins.h"
#include "sensors.h"
#include "settings.h"
#include "state_machine.h"
#include "time_utils.h"

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

  // color mapping for water level
  // below 25% = last LED red 100%
  // 25% = last 2 orange 80%
  // 50% = last 4 yellow 60%
  // 75% = last 6 blue 40%
  // 100% = all green 40%
  if (lvl < 25) {
    strip.setPixelColor(NEO_COUNT-1, strip.Color(255,0,0)); // red, 100%
  } else if (lvl < 50) {
    for (uint8_t i = NEO_COUNT-3; i < NEO_COUNT; i++)
      strip.setPixelColor(i, strip.Color(180,30,0));  // orange, 80%
  } else if (lvl < 75) {
    for (uint8_t i = NEO_COUNT-5; i < NEO_COUNT; i++)
      strip.setPixelColor(i, strip.Color(80,30,30)); // yellow, 60%
  } else if (lvl < 100) {
    for (uint8_t i = NEO_COUNT-7; i < NEO_COUNT; i++)
      strip.setPixelColor(i, strip.Color(0,0,80)); // blue, 40%
  } else {
    for (uint8_t i = 0; i < NEO_COUNT; i++)
      strip.setPixelColor(i, strip.Color(0,50,0)); // green, 40%
  }
  strip.show();
#endif
}
