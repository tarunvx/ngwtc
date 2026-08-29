/*
  ============================================================
   SWTC — ESP-NOW RSSI RANGE TEST : LOCAL NODE  (ESP32 WROOM-32)
  ============================================================

   Pair this with tests/rssi-test-tank-node (ESP8266). Purpose: find out
   whether the poor ESP-NOW range is a radio/antenna fault or just path
   loss, by measuring actual signal strength instead of guessing.

   This node LISTENS for PING frames, reports the RSSI it measured, and
   echoes that value back in a PONG so the tank node can see its own
   uplink strength too.

   ----------------------------------------------------------------
   HOW TO USE
   ----------------------------------------------------------------
   1. Flash this to the ESP32, and rssi-test-tank-node to the NodeMCU.
      TEST_CHANNEL must match on both (fixed channel keeps the router
      out of the experiment).
   2. Place the two boards ~1 m apart, both powered normally.
   3. Read the once-per-second summary and walk the tank node out to
      5 m, 10 m, 15 m, noting rssi_avg and loss% at each stop.

   ----------------------------------------------------------------
   INTERPRETING rssi AT 1 METRE
   ----------------------------------------------------------------
     -30 .. -45 dBm  radios are HEALTHY -> range loss is path/obstacles
     -60 .. -75 dBm  something is WRONG -> detuned antenna, low TX power,
                     or a faulty module. Chasing obstacles won't help.

   Healthy free-space falloff is about -6 dB per doubling of distance
   (1 m -> 2 m -> 4 m ...). Much steeper means absorption/reflection;
   starting low and staying flat means a hardware fault.

   Practical link margin: packets get unreliable below about -85 dBm.

   Board: "ESP32 Dev Module"  |  115200 baud
  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define TEST_CHANNEL   1        // MUST match the tank node
#define PKT_MAGIC      0xA7
#define PKT_PING       1
#define PKT_PONG       2
#define REPORT_MS      1000

typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;
  int8_t   rssi;   // in a PONG: the RSSI this node measured for the PING
} RssiPkt;

static uint8_t s_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Window stats (written in the RX callback, drained once per second)
static volatile uint32_t s_rxCount   = 0;
static volatile int32_t  s_rssiSum   = 0;
static volatile int8_t   s_rssiMin   = 0;
static volatile int8_t   s_rssiMax   = 0;
static volatile int8_t   s_rssiLast  = 0;
static volatile uint32_t s_drops     = 0;
static volatile uint16_t s_prevSeq   = 0;
static volatile bool     s_havePrev  = false;

// PONG is queued here and sent from loop() — esp_now_send() must not be
// called from inside the receive callback.
static volatile bool     s_pongDue   = false;
static volatile uint16_t s_pongSeq   = 0;
static volatile int8_t   s_pongRssi  = 0;

static void handleFrame(const uint8_t* data, int len, int8_t rssi) {
  if (len != (int)sizeof(RssiPkt)) return;
  RssiPkt p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != PKT_MAGIC || p.type != PKT_PING) return;

  if (s_havePrev) {
    uint16_t expected = (uint16_t)(s_prevSeq + 1);
    if (p.seq != expected) s_drops += (uint16_t)(p.seq - expected);
  }
  s_prevSeq  = p.seq;
  s_havePrev = true;

  if (s_rxCount == 0) { s_rssiMin = rssi; s_rssiMax = rssi; }
  else {
    if (rssi < s_rssiMin) s_rssiMin = rssi;
    if (rssi > s_rssiMax) s_rssiMax = rssi;
  }
  s_rssiSum += rssi;
  s_rssiLast = rssi;
  s_rxCount++;

  s_pongSeq  = p.seq;
  s_pongRssi = rssi;
  s_pongDue  = true;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  handleFrame(data, len, info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0);
}
#else
// Core 2.x cannot surface per-packet RSSI — upgrade to core 3.x for this test.
static void onRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
  handleFrame(data, len, 0);
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("   ESP-NOW RSSI TEST — LOCAL NODE (ESP32, RX)"));
  Serial.println(F("=================================================="));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(TEST_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(84);          // 84 = 21 dBm (max)

  Serial.print(F("  MAC: "));
  Serial.println(WiFi.macAddress());
  Serial.printf("  channel=%d  tx_power=max\n", TEST_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("  ESP-NOW init FAILED"));
    return;
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, s_broadcast, 6);
  peer.channel = TEST_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

#if !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
  Serial.println(F("  WARNING: core 2.x — RSSI unavailable, will read 0"));
#endif
  Serial.println(F("  waiting for PING frames..."));
  Serial.println(F("--------------------------------------------------"));
}

void loop() {
  if (s_pongDue) {
    s_pongDue = false;
    RssiPkt p{ PKT_MAGIC, PKT_PONG, s_pongSeq, s_pongRssi };
    esp_now_send(s_broadcast, (uint8_t*)&p, sizeof(p));
  }

  static uint32_t lastReport = 0;
  if (millis() - lastReport < REPORT_MS) return;
  lastReport = millis();

  noInterrupts();
  uint32_t n    = s_rxCount;
  int32_t  sum  = s_rssiSum;
  int8_t   mn   = s_rssiMin, mx = s_rssiMax, last = s_rssiLast;
  uint32_t drop = s_drops;
  s_rxCount = 0; s_rssiSum = 0;
  interrupts();

  if (n == 0) {
    Serial.printf("[--] no packets this second  (total drops=%lu)  "
                  "-> out of range / channel mismatch / tank node off\n",
                  (unsigned long)drop);
    return;
  }

  float avg = (float)sum / (float)n;
  Serial.printf("rx=%2lu/s  rssi avg=%.1f  min=%d  max=%d  last=%d dBm  drops=%lu\n",
                (unsigned long)n, avg, mn, mx, last, (unsigned long)drop);
}
