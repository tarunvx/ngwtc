#ifndef UI_H
#define UI_H
#include <stdint.h>
void ui_init();
void ui_tick();
void ui_requestUpdate();
void ui_showPopup(const char* msg, uint16_t durationMs = 1500);
void ui_toggleOpScreen();  // toggle between operating screen and home
bool ui_isOpScreen();      // true when operating screen is shown
void ui_wake();            // full brightness + restart the idle-dim timer
bool ui_isDim();           // true while the panel is dimmed
bool ui_diagMode();        // continuous diagnostic screen (NVS-backed)
void ui_setDiagMode(bool on);
void ui_toggleDiagMode();
uint8_t ui_diagPage();     // 0 = link/system, 1 = live sensors
void ui_setDiagPage(uint8_t page);
void ui_nextDiagPage(int8_t delta);
#endif
