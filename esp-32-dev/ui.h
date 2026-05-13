#ifndef UI_H
#define UI_H
#include <stdint.h>
void ui_init();
void ui_tick();
void ui_requestUpdate();
void ui_showPopup(const char* msg, uint16_t durationMs = 1500);
#endif
