/*

  Project - Smart Water Tank Monitor & Controller v5.0

  Author: Tarun Vishwakarma

  Start - 19th April 2026

*/

#include <Arduino.h> 
#include <WiFi.h>
#include "config.h" 
#include "settings.h" 
#include "fault_log.h"
#include "event_queue.h"
#include "sensors.h" 
#include "actuator.h"
#include "buttons.h"
#include "ui.h"
#include "led.h"
#include "buzzer.h"
#include "mqtt.h"
#include "state_machine.h"
#include "tasks.h"
#include "menu.h"

static void initNTP() {
  // Configure NTP — IST = UTC+5:30
  configTime(5 * 3600 + 30 * 60, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println(F("[NTP] sync requested (IST)"));
}

void setup() {

  Serial.begin(115200);
  delay(200);
  Serial.println();

  Serial.println(F("=== SWTC v5.0 boot ==="));

  settings_init();
  faultlog_init();
  initEventQueue();
  sensors_init();
  actuator_init();        
  buttons_init();
  buzzer_init();
  led_init();
  ui_init();
  mqtt_init();            // also connects WiFi
  initNTP();              // start NTP after WiFi
  sm_init();
  menu_init();
  initTasks();

  Serial.println(F("=== boot complete ==="));
}


void loop() {

  // After setup(), all real work runs in pinned FreeRTOS tasks.
  // Arduino's loop() runs as a low-priority task on Core 1 — we reuse it
  // as a tiny Serial CMD console (line-ended) for testing without MQTT.
  // Examples:  CMD:1:MODE:AUTO   CMD:2:GET:STATUS   CMD:3:SYS:RESET_FAULTS

  static char buf[128]; static uint8_t i = 0;

  while (Serial.available()) {

    char c = Serial.read();
    if (c == '\n' || c == '\r') {

      if (i) { buf[i] = 0; mqtt_dispatchCmd(buf); i = 0; }

    } else if (i < sizeof(buf) - 1) {

      buf[i++] = c;

    }

  }

  vTaskDelay(pdMS_TO_TICKS(50));

}