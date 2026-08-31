#include "ota.h"
#include "config.h"

#if OTA_ENABLED

#include <WiFi.h>
#include <ArduinoOTA.h>
#include "SECRETS.h"
#include "tasks.h"
#include "state_machine.h"
#include "actuator.h"
#include "ui.h"
#include "led.h"
#include "buzzer.h"

#ifndef OTA_HOSTNAME
  #define OTA_HOSTNAME "swtc-local"
#endif

static bool s_started  = false;   // ArduinoOTA.begin() done
static bool s_inProg   = false;
static int  s_lastPct  = -1;

// Refuse to flash while the pump is being driven — an update reboots the board,
// and a reboot mid-pulse would leave the latching relay in an unknown state.
static bool safeToUpdate() {
  SystemState st = sm_state();
  return !sm_isPumpRunningState(st)
      && st != ST_STARTING
      && st != ST_STOPPING
      && !actuator_isPulsing();
}

void ota_init() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

  ArduinoOTA.onStart([]() {
    s_inProg  = true;
    s_lastPct = -1;
    Serial.println(F("[OTA] start — suspending tasks"));
    ui_showPopup("OTA UPDATING", 60000);
    buzzer_chirp(300);
    vTaskDelay(pdMS_TO_TICKS(200));   // let UI/buzzer render before they stop
    // Flash writes park the other core; the WDT-registered tasks would trip.
    tasks_prepareForOta();
    led_blank();
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int pct = total ? (int)((done * 100UL) / total) : 0;
    if (pct != s_lastPct && pct % 10 == 0) {
      s_lastPct = pct;
      Serial.printf("[OTA] %d%%\n", pct);
    }
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("[OTA] complete — rebooting"));
    Serial.flush();
  });

  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("[OTA] error %u — resuming tasks\n", (unsigned)err);
    s_inProg = false;
    tasks_resumeAfterOta();
  });

  ArduinoOTA.begin();
  s_started = true;
  Serial.printf("[OTA] listening as %s at %s\n",
    OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void ota_tick() {
  if (!s_started || WiFi.status() != WL_CONNECTED) return;
  // Gate at handle() so an upload simply cannot begin while the pump runs.
  if (!s_inProg && !safeToUpdate()) return;
  ArduinoOTA.handle();
}

bool ota_inProgress() { return s_inProg; }

#else   // OTA_ENABLED == 0

void ota_init() {}
void ota_tick() {}
bool ota_inProgress() { return false; }

#endif
