#include "link.h"
#include "link_proto.h"
#include "config.h"
#include "events.h"
#include "event_queue.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ============================================================
//  ESP-NOW receive layer for the LOCAL NODE (ESP32).
//
//  The receive callback runs in the WiFi task context. We copy validated
//  frames into a snapshot guarded by a portMUX spinlock; all liveness
//  edge-detection and event emission happens in link_tick() (Control task)
//  to keep the callback short and non-blocking.
// ============================================================

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Shared snapshot (written in callback, read in accessors) — guarded by s_mux.
static volatile uint8_t  s_floatBits   = 0;
static volatile uint16_t s_distanceMm  = 0;
static volatile uint8_t  s_usLevelPct  = 0;
static volatile uint32_t s_flowTotal   = 0;
static volatile uint16_t s_seq         = 0;
static volatile uint8_t  s_flags       = 0;
static volatile uint32_t s_lastRxMs    = 0;
static volatile bool     s_everRx      = false;
static volatile uint32_t s_dropCount   = 0;
static volatile uint16_t s_prevSeq     = 0;
static volatile bool     s_havePrevSeq = false;

// Liveness edge tracking (link_tick only)
static bool     s_alivePrev = false;
static bool     s_linkInit  = false;

// ---- ESP-NOW receive callback ------------------------------
// Arduino-ESP32 core 3.x changed the callback signature to take an
// esp_now_recv_info_t*; core 2.x passes the raw MAC. Support both.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
#else
static void onRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
#endif
  if (len != (int)sizeof(LinkTelemetry)) return;

  LinkTelemetry t;
  memcpy(&t, data, sizeof(t));
  if (!link_checkFrame(&t, (size_t)len)) return;   // bad netId/version/CRC

  portENTER_CRITICAL_ISR(&s_mux);
  // Sequence-gap accounting (diagnostics only).
  if (s_havePrevSeq) {
    uint16_t expected = (uint16_t)(s_prevSeq + 1);
    if (t.seq != expected) {
      uint16_t gap = (uint16_t)(t.seq - expected);
      s_dropCount += gap;
    }
  }
  s_prevSeq     = t.seq;
  s_havePrevSeq = true;

  s_floatBits   = t.floatBits;
  s_distanceMm  = t.distanceMm;
  s_usLevelPct  = t.usLevelPct;
  s_flowTotal   = t.flowTotal;
  s_seq         = t.seq;
  s_flags       = t.flags;
  s_lastRxMs    = millis();
  s_everRx      = true;
  portEXIT_CRITICAL_ISR(&s_mux);
}

void link_init() {
  // WiFi must already be in STA mode (mqtt_init() does WiFi.mode(WIFI_STA)).
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("[LINK] ESP-NOW init FAILED"));
    return;
  }
  esp_now_register_recv_cb(onRecv);
  s_linkInit  = true;
  s_alivePrev = false;
  Serial.print(F("[LINK] ESP-NOW ready. Local MAC: "));
  Serial.println(WiFi.macAddress());
  Serial.println(F("[LINK] waiting for tank-node telemetry..."));
}

void link_tick() {
  if (!s_linkInit) return;
  bool alive = link_alive();
  if (alive != s_alivePrev) {
    s_alivePrev = alive;
    if (alive) {
      Serial.printf("[LINK] UP (seq=%u)\n", (unsigned)link_seq());
      Event e{}; e.type = EV_LINK_UP; e.p.u32 = link_seq();
      sendEvent(e);
    } else {
      uint32_t age = link_ageMs();
      Serial.printf("[LINK] DOWN (age=%lums, drops=%lu)\n",
        (unsigned long)age, (unsigned long)link_dropCount());
      Event e{}; e.type = EV_LINK_DOWN; e.p.u32 = age;
      sendEvent(e);
    }
  }
}

// ---- Liveness ----------------------------------------------
bool link_everReceived() {
  portENTER_CRITICAL(&s_mux);
  bool v = s_everRx;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

uint32_t link_ageMs() {
  portENTER_CRITICAL(&s_mux);
  bool ever = s_everRx;
  uint32_t last = s_lastRxMs;
  portEXIT_CRITICAL(&s_mux);
  if (!ever) return UINT32_MAX;
  return (uint32_t)(millis() - last);
}

bool link_alive() {
  return link_ageMs() < LINK_TIMEOUT_MS;
}

// ---- Snapshot accessors ------------------------------------
uint8_t link_floatBits() {
  portENTER_CRITICAL(&s_mux);
  uint8_t v = s_floatBits;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

bool link_levelPlausible() {
  uint8_t b = link_floatBits();
  bool l25  = b & LINK_FLOAT_25;
  bool l50  = b & LINK_FLOAT_50;
  bool l75  = b & LINK_FLOAT_75;
  bool l100 = b & LINK_FLOAT_100;
  // Monotonic: a higher float can only be wet if all lower ones are too.
  return !((l100 && (!l75 || !l50 || !l25)) ||
           (l75  && (!l50 || !l25)) ||
           (l50  && !l25));
}

uint8_t link_levelPct() {
  uint8_t b = link_floatBits();
  if (b & LINK_FLOAT_100) return 100;
  if (b & LINK_FLOAT_75)  return 75;
  if (b & LINK_FLOAT_50)  return 50;
  if (b & LINK_FLOAT_25)  return 25;
  return 0;
}

uint16_t link_distanceMm() {
  portENTER_CRITICAL(&s_mux);
  uint16_t v = s_distanceMm;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

uint8_t link_usLevelPct() {
  portENTER_CRITICAL(&s_mux);
  uint8_t v = s_usLevelPct;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

uint32_t link_flowTotalPulses() {
  portENTER_CRITICAL(&s_mux);
  uint32_t v = s_flowTotal;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

uint16_t link_seq() {
  portENTER_CRITICAL(&s_mux);
  uint16_t v = s_seq;
  portEXIT_CRITICAL(&s_mux);
  return v;
}

uint32_t link_dropCount() {
  portENTER_CRITICAL(&s_mux);
  uint32_t v = s_dropCount;
  portEXIT_CRITICAL(&s_mux);
  return v;
}
