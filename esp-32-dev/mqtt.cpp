#include "mqtt.h"
#include "config.h"
#include "events.h"
#include "event_queue.h"
#include "settings.h"
#include "state_machine.h"
#include "sensors.h"
#include "fault_log.h"
#include "time_utils.h"

#if HAS_MQTT
  #include <WiFi.h>
  #include "Adafruit_MQTT.h"
  #include "Adafruit_MQTT_Client.h"
  static WiFiClient            s_wifi;
  static Adafruit_MQTT_Client  s_mqtt(&s_wifi, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_KEY);
  static Adafruit_MQTT_Publish   s_pubStatus(&s_mqtt, MQTT_FEED_BASE "/next-gen-water-tank-controller.swtc-slash-status");
  static Adafruit_MQTT_Publish   s_pubAck   (&s_mqtt, MQTT_FEED_BASE "/next-gen-water-tank-controller.swtc-slash-ack");
  static Adafruit_MQTT_Publish   s_pubLevel (&s_mqtt, MQTT_FEED_BASE "/next-gen-water-tank-controller.swtc-slash-level");
  static Adafruit_MQTT_Subscribe s_subCmd   (&s_mqtt, MQTT_FEED_BASE "/next-gen-water-tank-controller.swtc-slash-cmd");

  static uint32_t s_lastConnectTry = 0;
  static uint32_t s_backoff = 2000;
#endif

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
        settings_setU32("timer1Ms", mins * 60000UL);
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
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "FAULTS=%u", (unsigned)faultlog_count());
      mqtt_publishAck(id, true, tmp);
      faultlog_dump(Serial);
      return true;
    }
    if (strcmp(act, "STATUS") == 0) { mqtt_publishStatus(); mqtt_publishAck(id, true, "OK"); return true; }
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
  char line[96];
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
    "\"lvl\":%u,\"fl\":%u,\"i\":%u,\"io\":%u,\"f\":%u,"
    "\"t\":%d,\"rh\":%u,\"p\":%d,"
    "\"bi\":%u,\"bf\":%u,\"ov\":%u}",
    modeName(settings().mode), sm_stateName(sm_state()),
    settings().sleepMode ? 1u : 0u,
    sensors_levelPct(), sensors_flowLpmX10(),
    sensors_currentMv(), sensors_currentOffsetMv(), (unsigned)faultlog_count(),
    (int)sensors_tempCx10(), (unsigned)sensors_rhX10(),
    (int)(sensors_pressureMPa() * 1000),  // mPa integer for JSON simplicity
    settings().bypassCurrentSense ? 1u : 0u,
    settings().bypassFlowSense ? 1u : 0u,
    sm_overflowIgnore() ? 1u : 0u);
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
#endif

void mqtt_tick() {
#if HAS_MQTT
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
#endif
}
