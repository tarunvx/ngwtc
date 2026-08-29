#include "link.h"
#include "link_proto.h"
#include "config.h"
#include "pins.h"
#include "events.h"
#include "event_queue.h"

// ============================================================
//  Wired UART receive layer for the LOCAL NODE (ESP32).
//
//  Bytes are drained and parsed in link_tick() (Control task) rather than an
//  ISR, so no work happens in interrupt context. The snapshot is still
//  portMUX-guarded because the accessors are called from other tasks (UI,
//  MQTT) pinned to the other core.
// ============================================================

#define LINK_SERIAL  Serial1

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Shared snapshot (written by the parser, read by accessors) — guarded by s_mux.
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

// Frame parser state
static uint8_t  s_buf[sizeof(LinkTelemetry)];
static uint8_t  s_idx  = 0;
static uint8_t  s_sync = 0;   // 0=want netId, 1=want version, 2=want msgType, 3=body

static void acceptFrame(const LinkTelemetry* t) {
  portENTER_CRITICAL(&s_mux);
  // Sequence-gap accounting (diagnostics only).
  if (s_havePrevSeq) {
    uint16_t expected = (uint16_t)(s_prevSeq + 1);
    if (t->seq != expected) {
      uint16_t gap = (uint16_t)(t->seq - expected);
      s_dropCount += gap;
    }
  }
  s_prevSeq     = t->seq;
  s_havePrevSeq = true;

  s_floatBits   = t->floatBits;
  s_distanceMm  = t->distanceMm;
  s_usLevelPct  = t->usLevelPct;
  s_flowTotal   = t->flowTotal;
  s_seq         = t->seq;
  s_flags       = t->flags;
  s_lastRxMs    = millis();
  s_everRx      = true;
  portEXIT_CRITICAL(&s_mux);
}

// The three constant leading fields double as the frame preamble on a byte
// stream; a false match inside payload data just fails CRC and resyncs.
static void parseByte(uint8_t b) {
  switch (s_sync) {
    case 0:
      if (b == LINK_NET_ID) { s_buf[0] = b; s_sync = 1; }
      break;
    case 1:
      if      (b == LINK_PROTO_VERSION) { s_buf[1] = b; s_sync = 2; }
      else if (b == LINK_NET_ID)        { s_buf[0] = b; }
      else                              { s_sync = 0; }
      break;
    case 2:
      if      (b == LINK_MSG_TELEMETRY) { s_buf[2] = b; s_idx = 3; s_sync = 3; }
      else if (b == LINK_NET_ID)        { s_buf[0] = b; s_sync = 1; }
      else                              { s_sync = 0; }
      break;
    default:
      s_buf[s_idx++] = b;
      if (s_idx >= sizeof(LinkTelemetry)) {
        LinkTelemetry t;
        memcpy(&t, s_buf, sizeof(t));
        if (link_checkFrame(&t, sizeof(t))) acceptFrame(&t);
        s_sync = 0;
      }
      break;
  }
}

void link_init() {
  LINK_SERIAL.setRxBufferSize(256);
  LINK_SERIAL.begin(LINK_SERIAL_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);
  s_linkInit  = true;
  s_alivePrev = false;
  s_sync = 0;
  Serial.printf("[LINK] UART RX on GPIO%d @ %d baud\n",
    PIN_LINK_RX, LINK_SERIAL_BAUD);
  Serial.println(F("[LINK] waiting for tank-node telemetry..."));
}

void link_tick() {
  if (!s_linkInit) return;

  while (LINK_SERIAL.available()) parseByte((uint8_t)LINK_SERIAL.read());

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
