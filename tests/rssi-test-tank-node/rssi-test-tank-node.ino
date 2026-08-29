/*
  ============================================================
   SWTC — ESP-NOW RSSI RANGE TEST : TANK NODE  (NodeMCU ESP-12E / ESP8266)
  ============================================================

   Pair this with tests/rssi-test-local-node (ESP32). This node is the
   TRANSMITTER: it broadcasts PING frames and listens for the PONG, which
   carries the RSSI the ESP32 measured. So this serial output tells you the
   strength of the UPLINK — the direction that actually carries telemetry.

   It also reports the ESP-NOW send-callback result, which distinguishes
   "the radio never got it out" from "it was sent but nobody answered".

   ----------------------------------------------------------------
   HOW TO USE
   ----------------------------------------------------------------
   1. Flash this to the NodeMCU and rssi-test-local-node to the ESP32.
      TEST_CHANNEL must match on both.
   2. Start ~1 m apart and confirm a healthy reading (see table below).
   3. Walk this node to 5 m, 10 m, 15 m and note rssi + loss% at each stop.
      Keep BOTH boards in the same orientation (antennas parallel and
      vertical) — mismatched polarisation alone can cost 20 dB.

   ----------------------------------------------------------------
   INTERPRETING rssi AT 1 METRE
   ----------------------------------------------------------------
     -30 .. -45 dBm  radios are HEALTHY -> range loss is path/obstacles
     -60 .. -75 dBm  something is WRONG -> detuned antenna, low TX power,
                     or a faulty module

   Packets get unreliable below roughly -85 dBm. Healthy falloff is about
   -6 dB per doubling of distance.

   Note: the ESP8266 cannot read the RSSI of frames it receives without a
   promiscuous-mode hack, so the downlink figure isn't shown here — the
   echoed uplink RSSI is the number that matters, and links are usually
   near-symmetric.

   Board: "NodeMCU 1.0 (ESP-12E Module)"  |  115200 baud
  ============================================================
*/

#include <ESP8266WiFi.h>
#include <espnow.h>

#define TEST_CHANNEL   1        // MUST match the local node
#define PING_PERIOD_MS 100      // 10 pings/sec
#define REPORT_MS      1000
#define PKT_MAGIC      0xA7
#define PKT_PING       1
#define PKT_PONG       2

typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;
  int8_t   rssi;   // in a PONG: RSSI the ESP32 measured for our PING
} RssiPkt;

static uint8_t s_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint16_t s_seq       = 0;
static uint32_t s_sent      = 0;   // pings issued this window
static uint32_t s_sendOk    = 0;   // radio accepted/ack'd this window
static uint32_t s_pongs     = 0;   // replies heard this window
static int32_t  s_rssiSum   = 0;
static int8_t   s_rssiMin   = 0;
static int8_t   s_rssiMax   = 0;
static int8_t   s_rssiLast  = 0;
static bool     s_haveRssi  = false;

static void onSent(uint8_t* /*mac*/, uint8_t status) {
  if (status == 0) s_sendOk++;
}

static void onRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
  if (len != sizeof(RssiPkt)) return;
  RssiPkt p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != PKT_MAGIC || p.type != PKT_PONG) return;

  if (!s_haveRssi) { s_rssiMin = p.rssi; s_rssiMax = p.rssi; s_haveRssi = true; }
  else {
    if (p.rssi < s_rssiMin) s_rssiMin = p.rssi;
    if (p.rssi > s_rssiMax) s_rssiMax = p.rssi;
  }
  s_rssiSum += p.rssi;
  s_rssiLast = p.rssi;
  s_pongs++;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("   ESP-NOW RSSI TEST — TANK NODE (ESP8266, TX)"));
  Serial.println(F("=================================================="));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setOutputPower(20.5);            // max TX power
  wifi_set_channel(TEST_CHANNEL);

  Serial.print(F("  MAC: "));
  Serial.println(WiFi.macAddress());
  Serial.printf("  channel=%d  tx_power=max  ping=%dms\n",
                TEST_CHANNEL, PING_PERIOD_MS);

  if (esp_now_init() != 0) {
    Serial.println(F("  ESP-NOW init FAILED — rebooting"));
    delay(1000);
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);   // send PINGs + receive PONGs
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);
  esp_now_add_peer(s_broadcast, ESP_NOW_ROLE_COMBO, TEST_CHANNEL, NULL, 0);

  Serial.println(F("  broadcasting PINGs..."));
  Serial.println(F("--------------------------------------------------"));
}

void loop() {
  static uint32_t lastPing = 0;
  uint32_t now = millis();

  if (now - lastPing >= PING_PERIOD_MS) {
    lastPing = now;
    RssiPkt p{ PKT_MAGIC, PKT_PING, s_seq++, 0 };
    esp_now_send(s_broadcast, (uint8_t*)&p, sizeof(p));
    s_sent++;
  }

  static uint32_t lastReport = 0;
  if (now - lastReport < REPORT_MS) return;
  lastReport = now;

  uint32_t sent = s_sent, ok = s_sendOk, pongs = s_pongs;
  int32_t  sum  = s_rssiSum;
  int8_t   mn = s_rssiMin, mx = s_rssiMax, last = s_rssiLast;
  s_sent = s_sendOk = s_pongs = 0;
  s_rssiSum = 0;

  if (pongs == 0) {
    Serial.printf("[--] sent=%lu (radio ok=%lu)  NO PONG  "
                  "-> out of range / channel mismatch / local node off\n",
                  (unsigned long)sent, (unsigned long)ok);
    s_haveRssi = false;
    return;
  }

  float avg  = (float)sum / (float)pongs;
  float loss = sent ? (100.0f * (float)(sent - pongs) / (float)sent) : 0.0f;

  Serial.printf("uplink rssi avg=%.1f  min=%d  max=%d  last=%d dBm  |  "
                "sent=%lu  pong=%lu  loss=%.0f%%  (radio ok=%lu)\n",
                avg, mn, mx, last,
                (unsigned long)sent, (unsigned long)pongs, loss,
                (unsigned long)ok);
}
