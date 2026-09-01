/*
  ============================================================
   MINIMAL LOCAL NODE — power / stability isolation test
  ============================================================

   Purpose: strip the local node down to the smallest thing that still
   exercises the suspect hardware, so a reset here means the problem is
   NOT in the application logic.

   DELIBERATELY EXCLUDED (each was a suspect at some point):
     - NVS / Preferences   (flash writes park the other core)
     - FreeRTOS tasks      (everything runs in loop(), no task watchdog)
     - DHT22               (library bit-bangs with interrupts disabled)
     - NeoPixel            (3.3 V data into a 5 V strip is out of spec)
     - Buzzer              (was loading the 3V3 rail)
     - Relays / actuator   (never drives the pump — safe to leave running)
     - State machine, menu, buttons, fault log, OTA

   KEPT (so the test is still representative):
     - WiFi + MQTT         (the big current consumer, ~300-500 mA bursts)
     - OLED
     - Tank link UART RX + CRC parsing

   HOW TO READ THE RESULT:
     Still resets  -> not application logic. Power/hardware.
     Runs forever  -> the fault lives in what the full firmware adds.

   The reset reason and uptime are published every cycle; that is the
   actual output of this test.

   Board: "ESP32 Dev Module"   |   monitor at 115200
  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_system.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// Pulled in by relative path so there is no extra copy to drift out of sync.
#include "../../v6/local-node/link_proto.h"
#include "../../v6/local-node/SECRETS.h"

#define FW_MIN         "min-1.4.0"

#define FLOW_KPPL      450     // YF-S201 pulses per litre
// At ~3 L/min a 250 ms frame carries only ~6 pulses, so +/-1 pulse is +/-18%.
// Diffing the cumulative counter over a longer window fixes the resolution and
// is immune to a dropped frame (the next good one still carries the total).
#define FLOW_WINDOW_MS 3000

// Must match the tank node. link_proto.h currently defines 2400.
#define LINK_RX_PIN    32
#define LINK_TX_PIN    33      // must be a REAL pin: -1 leaves UART1 TX on the
                               // SPI flash pin (GPIO10) and the board boot-loops
#define PIN_SDA        21
#define PIN_SCL        22
#define OLED_ADDR      0x3C

#define LINK_TIMEOUT_MS   2000
#define OLED_PERIOD_MS     500
#define PUB_PERIOD_MS    15000
#define BOOT_RETRY_MS     3000   // retry the boot event faster than normal status

// Survives a watchdog/panic reset but not a power cycle, so the counter tells
// you how many times it restarted on its own overnight.
#define BOOT_MAGIC     0xB007C0DEUL
RTC_NOINIT_ATTR static uint32_t s_bootMagic;
RTC_NOINIT_ATTR static uint32_t s_bootCount;
static bool s_bootPublished = false;

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);

static WiFiClient           s_net;
static Adafruit_MQTT_Client s_mqtt(&s_net, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_KEY);
static Adafruit_MQTT_Publish s_pubStatus(&s_mqtt, MQTT_FEED_BASE "/ngwtc.status");

// ---- Link state -------------------------------------------
static uint8_t  s_buf[sizeof(LinkTelemetry)];
static uint8_t  s_idx  = 0;
static uint8_t  s_sync = 0;

static uint32_t s_rawBytes = 0;    // every byte seen: 0 = dead wire, >0 = traffic
static uint32_t s_crcErr   = 0;
static uint32_t s_drops    = 0;
static uint32_t s_tankRst  = 0;    // tank-node restarts, seen as seq going to 0
static uint32_t s_frames   = 0;
static uint32_t s_lastRxMs = 0;

// A forward seq jump bigger than this (500 s at 4 Hz) is a restart, not loss.
#define SEQ_GAP_MAX   2000

static uint16_t s_seq        = 0;
static uint16_t s_prevSeq    = 0;
static bool     s_havePrev   = false;
static uint8_t  s_floatBits  = 0;
static uint8_t  s_usPct      = 0;
static uint16_t s_distMm     = 0;
static uint32_t s_tankUpMs   = 0;   // tank node's own uptime, from the last frame
static uint32_t s_flowTotal  = 0;
static uint32_t s_flowPrevTotal  = 0;
static uint32_t s_flowWindowMs   = 0;
static bool     s_flowPrimed     = false;
static uint16_t s_lpmX10     = 0;   // instantaneous rate, L/min x10

static bool s_oledOk = false;

static const char* resetName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "UNKNOWN";
  }
}

// Floats are a bitmap of the four level switches; report the highest wet one.
static uint8_t levelPct() {
  if (s_floatBits & LINK_FLOAT_100) return 100;
  if (s_floatBits & LINK_FLOAT_75)  return 75;
  if (s_floatBits & LINK_FLOAT_50)  return 50;
  if (s_floatBits & LINK_FLOAT_25)  return 25;
  return 0;
}

static bool linkAlive() {
  return s_frames && (millis() - s_lastRxMs) < LINK_TIMEOUT_MS;
}

static void acceptFrame(const LinkTelemetry* t) {
  if (s_havePrev) {
    uint16_t expected = (uint16_t)(s_prevSeq + 1);
    if (t->seq != expected) {
      uint16_t gap = (uint16_t)(t->seq - expected);
      // Tank node restarting resets seq to 0; unsigned maths turns that into a
      // ~65000 "gap". A natural uint16 wrap gives gap 0, so it is unaffected.
      if (gap > SEQ_GAP_MAX) s_tankRst++;
      else                   s_drops += gap;
    }
  }
  s_prevSeq   = t->seq;
  s_havePrev  = true;
  s_seq       = t->seq;
  s_floatBits = t->floatBits;
  s_usPct     = t->usLevelPct;
  s_distMm    = t->distanceMm;
  s_tankUpMs  = t->uptimeMs;
  s_flowTotal = t->flowTotal;
  s_lastRxMs  = millis();
  s_frames++;
}

// pulses/ms -> L/min x10:  x1000 ms/s x60 s/min x10 / 450 == x4000/3.
static void updateFlowRate() {
  uint32_t now = millis();

  if (!linkAlive()) { s_flowPrimed = false; s_lpmX10 = 0; return; }

  if (!s_flowPrimed) {
    s_flowPrevTotal = s_flowTotal;
    s_flowWindowMs  = now;
    s_flowPrimed    = true;
    return;
  }

  uint32_t elapsed = now - s_flowWindowMs;
  if (elapsed < FLOW_WINDOW_MS) return;

  uint32_t total  = s_flowTotal;
  uint32_t pulses = (total >= s_flowPrevTotal) ? (total - s_flowPrevTotal) : 0;
  s_flowPrevTotal = total;      // tank restart zeroes the total: re-baseline
  s_flowWindowMs  = now;

  s_lpmX10 = (uint16_t)((uint32_t)pulses * 4000UL / (3UL * elapsed));
}

// netId/version/msgType are constant, so they double as a 3-byte preamble.
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
        else                                s_crcErr++;
        s_sync = 0;
      }
      break;
  }
}

static void drawOled() {
  if (!s_oledOk) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.printf("MIN  up%lus", (unsigned long)(millis() / 1000));
  oled.setCursor(98, 0);
  oled.print(linkAlive() ? "LK" : "--");

  oled.setCursor(0, 12);
  oled.printf("LVL %3u%%  US %3u%%", levelPct(), s_usPct);

  oled.setCursor(0, 22);
  oled.printf("DIST %u mm", s_distMm);

  oled.setCursor(0, 32);
  oled.printf("FLOW %u.%u L/m", s_lpmX10 / 10, s_lpmX10 % 10);

  oled.setCursor(0, 42);
  oled.printf("SEQ %u DR %lu R%lu", s_seq, (unsigned long)s_drops,
              (unsigned long)s_tankRst);

  oled.setCursor(0, 52);
  oled.printf("RX %lu CE %lu", (unsigned long)s_rawBytes, (unsigned long)s_crcErr);

  oled.display();
}

static void mqttEnsure() {
  if (s_mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  s_mqtt.connect();   // blocking, but this loop has nothing else to do
}

// One-shot on each boot, retried until the broker actually takes it. Counting
// these in the feed is the point of the overnight run.
static void publishBoot() {
  if (s_bootPublished || !s_mqtt.connected()) return;
  char json[160];
  snprintf(json, sizeof(json),
    "{\"ev\":\"boot\",\"fw\":\"%s\",\"rst\":\"%s\",\"n\":%lu,\"heap\":%lu}",
    FW_MIN, resetName(), (unsigned long)s_bootCount, (unsigned long)ESP.getFreeHeap());
  Serial.printf("[BOOT] %s\n", json);
  if (s_pubStatus.publish(json)) s_bootPublished = true;
}

static void publishStatus() {
  char json[220];
  snprintf(json, sizeof(json),
    "{\"fw\":\"%s\",\"rst\":\"%s\",\"n\":%lu,\"up\":%lu,\"heap\":%lu,\"min\":%lu,"
    "\"lvl\":%u,\"us\":%u,\"d\":%u,\"fl\":%u,\"fp\":%lu,"
    "\"tu\":%lu,\"la\":%lu,"
    "\"ls\":%u,\"fr\":%lu,\"ld\":%lu,\"tr\":%lu,\"rx\":%lu,\"ce\":%lu,\"lk\":%u}",
    FW_MIN, resetName(),
    (unsigned long)s_bootCount,
    (unsigned long)(millis() / 1000),
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMinFreeHeap(),
    levelPct(), s_usPct, s_distMm,
    s_lpmX10,
    (unsigned long)s_flowTotal,
    (unsigned long)(s_tankUpMs / 1000),
    (unsigned long)(s_frames ? (millis() - s_lastRxMs) : 0),
    s_seq, (unsigned long)s_frames, (unsigned long)s_drops,
    (unsigned long)s_tankRst,
    (unsigned long)s_rawBytes, (unsigned long)s_crcErr,
    linkAlive() ? 1 : 0);

  Serial.printf("[PUB %u B] %s\n", (unsigned)strlen(json), json);
  if (s_mqtt.connected() && !s_pubStatus.publish(json)) {
    Serial.println(F("[PUB] FAILED"));
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);

  if (s_bootMagic != BOOT_MAGIC) {   // cleared by a power cycle
    s_bootMagic = BOOT_MAGIC;
    s_bootCount = 0;
  }
  s_bootCount++;

  Serial.println();
  Serial.println(F("=== MINIMAL LOCAL NODE (no NVS, no tasks) ==="));
  Serial.printf("fw=%s  last reset=%s  boot #%lu\n",
    FW_MIN, resetName(), (unsigned long)s_bootCount);

  Wire.begin(PIN_SDA, PIN_SCL);
  s_oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (s_oledOk) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(F("MINIMAL TEST"));
    oled.println(resetName());
    oled.display();
  } else {
    Serial.println(F("[OLED] not found"));
  }

  Serial1.setRxBufferSize(256);
  Serial1.begin(LINK_SERIAL_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  Serial.printf("[LINK] RX GPIO%d @ %d baud\n", LINK_RX_PIN, LINK_SERIAL_BAUD);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println(F("[WIFI] connecting"));
}

void loop() {
  while (Serial1.available()) {
    s_rawBytes++;
    parseByte((uint8_t)Serial1.read());
  }

  updateFlowRate();

  static uint32_t lastOled = 0;
  if (millis() - lastOled >= OLED_PERIOD_MS) {
    lastOled = millis();
    drawOled();
  }

  static uint32_t lastPub = 0;
  uint32_t period = s_bootPublished ? PUB_PERIOD_MS : BOOT_RETRY_MS;
  if (millis() - lastPub >= period) {
    lastPub = millis();
    mqttEnsure();
    publishBoot();
    if (s_bootPublished) publishStatus();
  }

  delay(2);
}
