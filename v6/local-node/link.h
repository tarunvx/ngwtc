#ifndef LINK_H
#define LINK_H

#include <Arduino.h>

// ============================================================
//  link.h — v6 wired tank link, receive side (LOCAL NODE / ESP32)
//
//  Receives LinkTelemetry frames sent by the tank node over a one-way
//  3.3 V UART (tank TX -> PIN_LINK_RX, common ground), validates them
//  (LINK_NET_ID + version + CRC16), and exposes the latest sensor snapshot
//  to the rest of the firmware. The sensors module sources floats/level/flow
//  from here instead of local GPIO.
//
//  Replaced ESP-NOW in v6.1: the radio link measured -93 dBm with ~66%
//  packet loss at the installed positions, and its attenuation varied with
//  tank level. The wire is deterministic and reuses the existing cable.
//
//  Liveness: if no valid frame arrives within LINK_TIMEOUT_MS, the link
//  is considered DOWN and link_tick() emits EV_LINK_DOWN (and EV_LINK_UP
//  when it recovers). The state machine fails safe on EV_LINK_DOWN.
// ============================================================

#define LINK_TIMEOUT_MS  2000   // no valid frame for 2s => link down (8 frames)

void     link_init();           // opens the UART; no WiFi dependency
void     link_tick();           // periodic: drain UART, parse, liveness events

// ---- Liveness ----------------------------------------------
bool     link_alive();          // true if a valid frame arrived within timeout
bool     link_everReceived();   // true once any valid frame has been seen
uint32_t link_ageMs();          // ms since last valid frame (UINT32_MAX if none)

// ---- Latest snapshot (thread-safe accessors) ---------------
uint8_t  link_floatBits();      // LINK_FLOAT_* bitmap from tank node
uint8_t  link_levelPct();       // resolved 0/25/50/75/100 (monotonic)
bool     link_levelPlausible(); // false if float pattern non-monotonic
uint16_t link_distanceMm();     // ultrasonic air gap in mm (0 = no echo)
uint8_t  link_usLevelPct();     // ultrasonic level 0..100% (mapped on tank node)
uint32_t link_flowTotalPulses();// cumulative pulses since tank-node boot
uint16_t link_seq();            // last sequence number
uint32_t link_dropCount();
uint32_t link_rawBytes();      // detected sequence gaps (diagnostics)

#endif // LINK_H
