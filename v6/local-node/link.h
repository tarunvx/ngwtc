#ifndef LINK_H
#define LINK_H

#include <Arduino.h>

// ============================================================
//  link.h — v6 ESP-NOW receive layer (LOCAL NODE / ESP32)
//
//  Receives LinkTelemetry frames broadcast by the tank node, validates
//  them (LINK_NET_ID + version + CRC16), and exposes the latest sensor
//  snapshot to the rest of the firmware. The sensors module sources
//  floats/pressure/flow from here instead of local GPIO.
//
//  Liveness: if no valid frame arrives within LINK_TIMEOUT_MS, the link
//  is considered DOWN and link_tick() emits EV_LINK_DOWN (and EV_LINK_UP
//  when it recovers). The state machine fails safe on EV_LINK_DOWN.
// ============================================================

#define LINK_TIMEOUT_MS  1000   // no valid frame for 1s => link down (4 frames)

void     link_init();           // call AFTER WiFi is in STA mode (post mqtt_init)
void     link_tick();           // periodic: liveness edge detection + events

// ---- Liveness ----------------------------------------------
bool     link_alive();          // true if a valid frame arrived within timeout
bool     link_everReceived();   // true once any valid frame has been seen
uint32_t link_ageMs();          // ms since last valid frame (UINT32_MAX if none)

// ---- Latest snapshot (thread-safe accessors) ---------------
uint8_t  link_floatBits();      // LINK_FLOAT_* bitmap from tank node
uint8_t  link_levelPct();       // resolved 0/25/50/75/100 (monotonic)
bool     link_levelPlausible(); // false if float pattern non-monotonic
uint16_t link_pressureMv();     // native sensor millivolts (500..4500)
uint32_t link_flowTotalPulses();// cumulative pulses since tank-node boot
uint16_t link_seq();            // last sequence number
uint32_t link_dropCount();      // detected sequence gaps (diagnostics)

#endif // LINK_H
