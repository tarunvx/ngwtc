#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

// Network firmware update (ArduinoOTA). Call ota_init() after WiFi is up and
// ota_tick() from a task that is NOT watchdog-registered.
void ota_init();
void ota_tick();
bool ota_inProgress();

#endif
