#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

void sensors_init();

// Snapshot accessors (thread-safe via atomics on ESP32 word reads).
uint8_t  sensors_levelPct();
bool     sensors_currentPresent();
uint16_t sensors_currentMv();
uint16_t sensors_currentOffsetMv();  // measured CT bias mid-rail (diagnostics)
uint16_t sensors_currentAmps();  // CT mV scaled to whole amps (display only)
uint16_t sensors_flowLpmX10();
uint16_t sensors_flowThreshX10();   // configured "flow present" cutoff (NVS flowNoFlowThresh, min 1)
uint32_t sensors_totalLitersX10();
bool     sensors_fbOn();
bool     sensors_fbOff();
int16_t  sensors_tempCx10();
uint16_t sensors_rhX10();
bool     sensors_levelPlausible();
float    sensors_pressureMPa();
uint8_t  sensors_pressurePct();  // pressure mapped to 0-100% tank level

// Run one tick of sensor task (called from sensorTask).
void sensors_tick();

#endif
