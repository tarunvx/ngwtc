#include "mqtt.h"
#include "config.h"
#include "events.h"
#include "event_queue.h"
#include "settings.h"
#include "state_machine.h"
#include "sensors.h"
#include "fault_log.h"
#include "breadcrumb.h"
#include "time_utils.h"
#include "link.h"

#if HAS_MQTT
  #include <WiFi.h>
  #include "Adafruit_MQTT.h"
  #include "Adafruit_MQTT_Client.h"
  static WiFiClient            s_wifi;
  static Adafruit_MQTT_Client  s_mqtt(&s_wifi, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_KEY);
  static Adafruit_MQTT_Publish   s_pubStatus(&s_mqtt, MQTT_FEED_BASE "/ngwtc.status");
  static Adafruit_MQTT_Publish   s_pubAck   (&s_mqtt, MQTT_FEED_BASE "/ngwtc.ack");
  static Adafruit_MQTT_Publish   s_pubLevel (&s_mqtt, MQTT_FEED_BASE "/ngwtc.level");
  static Adafruit_MQTT_Subscribe s_subCmd   (&s_mqtt, MQTT_FEED_BASE "/ngwtc.cmd");

  static uint32_t s_lastConnectTry = 0;
  static uint32_t s_backoff = 2000;
#endif

// ---------------- Fault streaming ----------------
// Faults ride the ack feed, one per message: Adafruit_MQTT builds packets in a
// small fixed buffer and silently truncates, so a whole-log dump would never
// survive. The `FAULT:` prefix keeps them separable from `ACK:` lines.
#define FAULT_PUB_GAP_MS 2000

static uint32_t s_faultPubSeq  = 0;   // faultlog_seq() value already published
static int32_t  s_faultReplay  = -1;  // >= 0 while streaming the log for GET:FAULTS
static uint32_t s_lastFaultPub = 0;

// ---------------- CMD parser ----------------

// Format: CMD:<id>:<DOMAIN>:<ACTION>[:<arg0>[:<arg1>]]
bool mqtt_dispatchCmd(const char* line) {
  if (!line || strncmp(line, "CMD:", 4) != 0) return false;

  char buf[128];
  strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;

  char* tokens[8] = {0};
  uint8_t n = 0;
  char* p = buf;
  while (n < 8 && p) {
    tokens[n++] = p;
    char* sep = strchr(p, ':');
    if (sep) { *sep = 0; p = sep + 1; } else { break; }
  }
  if (n < 4) { mqtt_publishAck(0, false, "MALFORMED"); return false; }

  uint16_t id  = (uint16_t)atoi(tokens[1]);
  const char* dom = tokens[2];
  const char* act = tokens[3];

  Event e{};

  if (strcmp(dom, "PUMP") == 0) {
    if (strcmp(act, "ON") == 0) {
#if MONITOR_ONLY_MODE && !ALLOW_MANUAL_ACTUATION
      mqtt_publishAck(id, false, "MONITOR_ONLY"); return true;
#endif
      // If in SLEEP, wake up first
      if (settings().sleepMode || sm_state() == ST_SLEEP) {
        settings_setBool("sleepMode", false);
        // Give state machine a tick to transition SLEEP→IDLE
        Event we{}; we.type = EV_MODE_REQ; we.p.i32 = MODE_MANUAL;
        sendEvent(we);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (sm_isPumpRunningState(sm_state()) || sm_state() == ST_STARTING) {
        mqtt_publishAck(id, false, "ALREADY_RUNNING"); return true;
      }
      if (sm_state() != ST_IDLE && !(sm_state() == ST_FULL && sm_overflowIgnore())) {
        // From FULL, a start is only allowed when the one-shot overflow
        // override has been armed (PUMP:OVERFLOW / B2-long / menu).
        mqtt_publishAck(id, false, "NOT_IDLE"); return true;
      }
      if (n >= 6 && strcmp(tokens[4], "TIMER") == 0) {
        uint32_t mins = atoi(tokens[5]);
        settings_setU16("timer1Sec", (uint16_t)(mins * 60));
        // Simulate timer button press → state machine handles STARTING
        e.type = EV_BUTTON; e.p.btn = { BTN2, PRESS_SHORT };
      } else {
        // Switch to MANUAL mode and start pump
        e.type = EV_BUTTON; e.p.btn = { BTN1, PRESS_LONG };
      }
      sendEvent(e);
      mqtt_publishAck(id, true, "OK");
      return true;
    }
    if (strcmp(act, "OFF") == 0) {
      // Hard stop regardless of state
      if (sm_isPumpRunningState(sm_state()) || sm_state() == ST_STARTING) {
        e.type = EV_BUTTON; e.p.btn = { BTN3, PRESS_LONG };
        sendEvent(e);
        mqtt_publishAck(id, true, "OK");
      } else {
        mqtt_publishAck(id, true, "NOT_RUNNING");
      }
      return true;
    }
    if (strcmp(act, "OVERFLOW") == 0) {
      // Arm (or PUMP:OVERFLOW:OFF to disarm) the one-shot overflow override so
      // the NEXT manual/timer run may deliberately run past full. Self-clears
      // on stop / after the arm timeout.
      bool on = !(n >= 5 && strcmp(tokens[4], "OFF") == 0);
      sm_setOverflowIgnore(on);
      mqtt_publishAck(id, true, on ? "OVERFLOW_ARMED" : "OVERFLOW_OFF");
      return true;
    }
    mqtt_publishAck(id, false, "UNKNOWN_ACTION"); return true;
  }

  if (strcmp(dom, "MODE") == 0) {
    int8_t m = -1;
    if      (strcmp(act, "AUTO")   == 0) m = MODE_AUTO;
    else if (strcmp(act, "MANUAL") == 0) m = MODE_MANUAL;
    else if (strcmp(act, "TIMER")  == 0) m = MODE_TIMER;
    else if (strcmp(act, "SLEEP")  == 0) m = MODE_SLEEP;
    else if (strcmp(act, "MAINT")  == 0) m = MODE_MAINTENANCE;
    else if (strcmp(act, "MD1")    == 0) m = MODE_MD1;
    if (m < 0) { mqtt_publishAck(id, false, "BAD_MODE"); return true; }
    e.type = EV_MODE_REQ; e.p.i32 = m;
    sendEvent(e);
    mqtt_publishAck(id, true, "OK");
    return true;
  }

  if (strcmp(dom, "SET") == 0) {
    if (n < 5) { mqtt_publishAck(id, false, "NO_VALUE"); return true; }
    uint32_t v = (uint32_t)atol(tokens[4]);
    bool ok = settings_setU32(act, v) || settings_setU16(act, (uint16_t)v)
           || settings_setU8(act, (uint8_t)v) || settings_setBool(act, v != 0);
    mqtt_publishAck(id, ok, ok ? "OK" : "UNKNOWN_KEY");
    return true;
  }

  if (strcmp(dom, "GET") == 0) {
    if (strcmp(act, "FAULTS") == 0) {
      size_t cnt = faultlog_count();
      s_faultReplay = cnt ? 0 : -1;      // mqtt_tick streams them, paced, to the fault feed
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "FAULTS=%u", (unsigned)cnt);
      mqtt_publishAck(id, true, tmp);
      faultlog_dump(Serial);
      return true;
    }
    if (strcmp(act, "STATUS") == 0) { mqtt_publishStatus(); mqtt_publishAck(id, true, "OK"); return true; }
    if (strcmp(act, "DIAG") == 0) {
      char tmp[140];
      snprintf(tmp, sizeof(tmp), "fw=%s rst=%s heap=%lu min=%lu up=%lus sup=%lu bc=%s rx=%lu tu=%lus",
        FIRMWARE_VERSION,
        faultlog_resetReasonName(), (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(), (unsigned long)(millis() / 1000),
        (unsigned long)faultlog_suppressed(), bc_report(),
        (unsigned long)link_rawBytes(),
        (unsigned long)(link_tankUptimeMs() / 1000));
      mqtt_publishAck(id, true, tmp);
      return true;
    }
    mqtt_publishAck(id, false, "UNKNOWN_GET"); return true;
  }

  if (strcmp(dom, "SYS") == 0) {
    if (strcmp(act, "REBOOT") == 0) {
      mqtt_publishAck(id, true, "REBOOTING");
      delay(200);
      ESP.restart();
    }
    if (strcmp(act, "RESET_FAULTS") == 0) {
      faultlog_clear();
      sm_clearLatched();
      s_faultReplay = -1; s_faultPubSeq = 0;
      mqtt_publishAck(id, true, "OK");
      return true;
    }
    if (strcmp(act, "DEFAULTS") == 0) {
      settings_resetDefaults();
      mqtt_publishAck(id, true, "OK");
      return true;
    }
    mqtt_publishAck(id, false, "UNKNOWN_SYS"); return true;
  }

  mqtt_publishAck(id, false, "UNKNOWN_DOMAIN");
  return true;
}

// ---------------- Lifecycle ----------------

void mqtt_publishAck(uint16_t id, bool ok, const char* msg) {
  char line[160];   // GET:DIAG is the longest ack; still under the Adafruit_MQTT limit
  snprintf(line, sizeof(line), "ACK:%u:%s:%s", (unsigned)id, ok ? "OK" : "ERR", msg ? msg : "");
  Serial.printf("%s %s\n", LOG_TAG_MQ, line);
#if HAS_MQTT
  if (s_mqtt.connected()) s_pubAck.publish(line);
#endif
}

void mqtt_publishStatus() {
  char json[512];
  // Keys are abbreviated and booleans sent as 1/0: Adafruit_MQTT builds each
  // packet in a small fixed buffer and silently truncates anything larger.
  snprintf(json, sizeof(json),
    "{\"md\":\"%s\",\"st\":\"%s\",\"slp\":%u,"
    "\"lvl\":%u,\"fl\":%u,\"i\":%u,\"ia\":%u,\"io\":%u,\"f\":%u,"
    "\"t\":%d,\"rh\":%u,\"d\":%u,\"us\":%u,"
    "\"bi\":%u,\"bf\":%u,\"ov\":%u,"
    "\"lk\":%u,\"la\":%lu,\"ls\":%u,\"ld\":%lu,\"ce\":%lu,\"tr\":%lu}",
    modeName(settings().mode), sm_stateName(sm_state()),
    settings().sleepMode ? 1u : 0u,
    sensors_levelPct(), sensors_flowLpmX10(),
    sensors_currentMv(), sensors_currentAmpsX10(),
    sensors_currentOffsetMv(), (unsigned)faultlog_count(),
    (int)sensors_tempCx10(), (unsigned)sensors_rhX10(),
    (unsigned)sensors_distanceMm(), (unsigned)sensors_ultrasonicLevelPct(),
    settings().bypassCurrentSense ? 1u : 0u,
    settings().bypassFlowSense ? 1u : 0u,
    sm_overflowIgnore() ? 1u : 0u,
    link_alive() ? 1u : 0u,
    (unsigned long)(link_everReceived() ? link_ageMs() : 0),
    (unsigned)link_seq(), (unsigned long)link_dropCount(),
    (unsigned long)link_crcErrors(),
    (unsigned long)link_tankRestarts());
  size_t jlen = strlen(json);
  Serial.printf("%s STATUS (%u B) %s\n", LOG_TAG_MQ, (unsigned)jlen, json);
#if HAS_MQTT
  if (s_mqtt.connected()) {
    // Adafruit_MQTT builds packets in a small fixed buffer (MAXBUFFERSIZE) and
    // silently truncates anything larger, so the serial log can look healthy
    // while the broker receives malformed JSON. Report the result.
    if (!s_pubStatus.publish(json)) {
      Serial.printf("%s STATUS publish FAILED (%u B - payload too long?)\n",
        LOG_TAG_MQ, (unsigned)jlen);
    }
    // Publish level to dedicated topic (for cross-system tank level sharing)
    char lvlBuf[8];
    snprintf(lvlBuf, sizeof(lvlBuf), "%u", sensors_levelPct());
    if (!s_pubLevel.publish(lvlBuf)) {
      Serial.printf("%s LEVEL publish FAILED\n", LOG_TAG_MQ);
    }
  }
#endif
}

void mqtt_init() {
#if HAS_MQTT
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  s_mqtt.subscribe(&s_subCmd);
  s_mqtt.will(MQTT_FEED_BASE "/status", "{\"online\":false}");
#endif
}

#if HAS_MQTT
static void connectIfNeeded() {
  if (s_mqtt.connected()) return;
  if (!elapsed(s_lastConnectTry, s_backoff)) return;
  s_lastConnectTry = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  int8_t r = s_mqtt.connect();
  if (r == 0) { s_backoff = 2000; mqtt_publishStatus(); }
  else        { uint32_t nb = s_backoff * 2; if (nb > 60000) nb = 60000; s_backoff = nb; }
}

static void publishFault(const FaultEntry& e, uint32_t ordinal, bool replay) {
  char line[192];
  snprintf(line, sizeof(line),
    "FAULT:{\"n\":%lu,\"rp\":%u,\"ts\":%lu,\"c\":%u,\"cn\":\"%s\","
    "\"sv\":%u,\"sn\":\"%s\",\"st\":\"%s\",\"lvl\":%u,\"fl\":%u,\"i\":%u}",
    (unsigned long)ordinal, replay ? 1u : 0u, (unsigned long)e.ts,
    (unsigned)e.code, faultCodeName(e.code),
    (unsigned)e.severity, faultSevName(e.severity),
    sm_stateName((SystemState)e.state),
    (unsigned)e.levelPct, (unsigned)e.flowLpmX10, (unsigned)e.currentMv);
  size_t jlen = strlen(line);
  Serial.printf("%s (%u B) %s\n", LOG_TAG_MQ, (unsigned)jlen, line);
  if (!s_pubAck.publish(line)) {
    Serial.printf("%s FAULT publish FAILED (%u B)\n", LOG_TAG_MQ, (unsigned)jlen);
  }
  s_lastFaultPub = millis();
}

static void faultPubTick() {
  if (!elapsed(s_lastFaultPub, FAULT_PUB_GAP_MS)) return;
  size_t cnt = faultlog_count();
  FaultEntry e;

  if (s_faultReplay >= 0) {                    // GET:FAULTS replay, oldest first
    if ((size_t)s_faultReplay >= cnt) { s_faultReplay = -1; return; }
    if (faultlog_get((size_t)s_faultReplay, e)) publishFault(e, (uint32_t)s_faultReplay + 1, true);
    if ((size_t)++s_faultReplay >= cnt) s_faultReplay = -1;
    return;
  }

  uint32_t seq = faultlog_seq();
  if (seq < s_faultPubSeq) s_faultPubSeq = seq;   // log was cleared
  if (seq == s_faultPubSeq || cnt == 0) return;

  uint32_t behind = seq - s_faultPubSeq;
  if (behind > cnt) behind = cnt;                 // older ones already rotated out
  if (faultlog_get(cnt - behind, e)) publishFault(e, (uint32_t)(cnt - behind) + 1, false);
  s_faultPubSeq = seq - behind + 1;
}
#endif

void mqtt_tick() {
#if HAS_MQTT
  static bool s_loggedChannel = false;
  if (!s_loggedChannel && WiFi.status() == WL_CONNECTED) {
    s_loggedChannel = true;
    Serial.printf("%s WiFi connected: SSID=%s ch=%d RSSI=%d IP=%s\n", LOG_TAG_MQ,
      WiFi.SSID().c_str(), WiFi.channel(), WiFi.RSSI(),
      WiFi.localIP().toString().c_str());
  }

  connectIfNeeded();
  if (!s_mqtt.connected()) return;            // safety: never call into broker funcs while down
  Adafruit_MQTT_Subscribe* sub;
  while ((sub = s_mqtt.readSubscription(10))) {
    if (sub == &s_subCmd && s_subCmd.lastread) {
      const char* line = (const char*)s_subCmd.lastread;
      mqtt_dispatchCmd(line);
    }
  }
  static uint32_t lastPub = 0;
  if (elapsed(lastPub, MQTT_PUBLISH_PERIOD_MS)) { lastPub = millis(); mqtt_publishStatus(); }
  faultPubTick();
#endif
}
