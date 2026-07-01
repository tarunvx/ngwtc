// ============================================================
//  link_proto.h — SWTC v6 wireless link protocol (shared)
//
//  Smart Water Tank Controller, two-node ESP-NOW design.
//
//   ┌────────────────────┐         ESP-NOW broadcast        ┌────────────────────┐
//   │   TANK NODE        │   ───────────────────────────►   │   LOCAL NODE       │
//   │   (ESP8266 ESP-12) │     LinkTelemetry @ ~4 Hz        │   (ESP32 brain)    │
//   │  floats/press/flow │                                  │  relays/CT/UI/MQTT │
//   └────────────────────┘                                  └────────────────────┘
//
//  This header is the SINGLE SOURCE OF TRUTH for the on-air packet layout.
//  It is intentionally dependency-free (only <stdint.h>/<stddef.h>) so the
//  identical file can be dropped into BOTH the ESP8266 tank-node sketch and
//  the ESP32 local-node sketch folders.
//
//  IMPORTANT: keep this file byte-for-byte identical in:
//     v6/shared/link_proto.h      (master copy)
//     v6/tank-node/link_proto.h   (build copy)
//     v6/local-node/link_proto.h  (build copy)
//
//  Both MCUs are 32-bit little-endian, so the __attribute__((packed)) struct
//  serializes identically on the wire. Do NOT reorder or resize fields without
//  bumping LINK_PROTO_VERSION (the receiver rejects mismatched versions).
//
//  Integrity: ESP-NOW encryption is NOT cross-compatible between ESP8266 and
//  ESP32, so the link runs UNENCRYPTED and we protect payloads with a CRC16.
//  Spoofing is mitigated by LINK_NET_ID + version + msgType filtering.
// ============================================================
#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stdint.h>
#include <stddef.h>

// ---- Protocol identity ------------------------------------
#define LINK_PROTO_VERSION  1      // bump on any struct layout change
#define LINK_NET_ID         0x57   // 'W' — private network tag, filters foreign packets
#define LINK_MSG_TELEMETRY  1      // msgType: tank → local sensor frame

// ---- Float bit map (bit set = float submerged / contact CLOSED) ----
//  Mirrors v5 active-LOW float wiring: water grounds the input.
//  The tank node resolves its active-LOW reads into these bits so the
//  local node's level logic stays byte-identical to v5.
#define LINK_FLOAT_25   0x01
#define LINK_FLOAT_50   0x02
#define LINK_FLOAT_75   0x04
#define LINK_FLOAT_100  0x08

// ---- Status flags -----------------------------------------
#define LINK_FLAG_PRESS_OK  0x01   // pressure ADC read looks valid
#define LINK_FLAG_FLOW_OK   0x02   // flow input wired / counting
#define LINK_FLAG_LOW_BATT  0x04   // reserved (tank node is mains powered)
#define LINK_FLAG_ACTIVE    0x08   // tank node sees flow above no-flow floor

// ============================================================
//  On-air telemetry frame (26 bytes). Sent ~every 250 ms.
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  netId;        // == LINK_NET_ID
  uint8_t  version;      // == LINK_PROTO_VERSION
  uint8_t  msgType;      // == LINK_MSG_TELEMETRY
  uint8_t  floatBits;    // LINK_FLOAT_* bitmap (resolved, active-LOW handled)
  uint16_t seq;          // rolls over; lets local node spot drops
  uint32_t uptimeMs;     // tank node millis() — diagnostics only
  uint8_t  flags;        // LINK_FLAG_* bitmap
  uint8_t  _pad;         // keep 16-bit fields aligned + reserved
  uint16_t pressureMv;   // pressure sensor output, scaled to its native mV
                         //   (0.5V..4.5V sensor => 500..4500). Local node
                         //   applies the SAME MPa formula as v5.
  uint16_t flowPulses;   // pulses counted in THIS telemetry window (diag)
  uint16_t flowWindowMs; // length of this window in ms (diag)
  uint32_t flowTotal;    // cumulative pulses since tank-node boot.
                         //   Local node differences this for robust LPM
                         //   (survives dropped packets).
  uint16_t vbattMv;      // reserved (0 when mains powered)
  uint16_t crc16;        // CRC16-CCITT over all preceding bytes
} LinkTelemetry;

// Compile-time guard: keep the frame within the ESP-NOW 250-byte limit.
typedef char _link_size_check[(sizeof(LinkTelemetry) <= 250) ? 1 : -1];

// ============================================================
//  CRC16-CCITT (poly 0x1021, init 0xFFFF). Header-only, static inline
//  so the identical definition can be included in multiple .cpp/.ino
//  translation units without multiple-definition linker errors.
// ============================================================
static inline uint16_t link_crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// Compute + store the CRC into a frame ready for sending.
static inline void link_fillCrc(LinkTelemetry* t) {
  t->crc16 = link_crc16((const uint8_t*)t, sizeof(LinkTelemetry) - sizeof(uint16_t));
}

// Validate a received frame: identity tags + CRC.
static inline bool link_checkFrame(const LinkTelemetry* t, size_t len) {
  if (len != sizeof(LinkTelemetry))      return false;
  if (t->netId   != LINK_NET_ID)         return false;
  if (t->version != LINK_PROTO_VERSION)  return false;
  if (t->msgType != LINK_MSG_TELEMETRY)  return false;
  uint16_t want = link_crc16((const uint8_t*)t, sizeof(LinkTelemetry) - sizeof(uint16_t));
  return want == t->crc16;
}

#endif // LINK_PROTO_H
