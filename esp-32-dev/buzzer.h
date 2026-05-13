#ifndef BUZZER_H
#define BUZZER_H
#include <Arduino.h>
enum BuzzPattern : uint8_t { BZ_NONE, BZ_SHORT, BZ_FULL, BZ_ERROR, BZ_LATCHED, BZ_OVERFLOW };
void buzzer_init();
void buzzer_set(BuzzPattern p);
void buzzer_tick();
void buzzer_silence();
#endif
