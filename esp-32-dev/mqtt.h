#ifndef MQTT_MOD_H
#define MQTT_MOD_H
#include <Arduino.h>

void mqtt_init();
void mqtt_tick();
void mqtt_publishStatus();
void mqtt_publishAck(uint16_t id, bool ok, const char* msg);

// Parse and dispatch a CMD line — usable from both MQTT and Serial.
// Returns true if syntactically valid.
bool mqtt_dispatchCmd(const char* line);

#endif
