#ifndef BUZZER_H
#define BUZZER_H
#include <Arduino.h>
enum BuzzPattern : uint8_t { BZ_NONE, BZ_SHORT, BZ_RUN, BZ_FULL, BZ_ERROR, BZ_LATCHED, BZ_OVERFLOW };
void buzzer_init();
void buzzer_set(BuzzPattern p);
void buzzer_tick();
void buzzer_silence();
void buzzer_autoTick();   // maps system state -> pattern automatically
void buzzer_chirp(uint16_t ms = 120);  // one-shot confirmation blip (overrides pattern briefly)
void buzzer_beep(uint16_t onMs);       // BLOCKING beep — boot/setup only (before tasks start)
#endif
