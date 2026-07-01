#include "tasks.h"
#include "config.h"
#include "events.h"
#include "event_queue.h"
#include "state_machine.h"
#include "actuator.h"
#include "sensors.h"
#include "safety.h"
#include "buttons.h"
#include "ui.h"
#include "led.h"
#include "buzzer.h"
#include "mqtt.h"
#include "link.h"
#include "selftest.h"
#include "fault_log.h"
#include "settings.h"
#include "time_utils.h"
#include <esp_task_wdt.h>
#if __has_include(<esp_idf_version.h>)
  #include <esp_idf_version.h>
#endif

static TaskHandle_t hSensor, hControl, hSafety, hButton, hUI, hLED, hBuzz, hMQTT;

// ============== CORE 1 ==============
static void sensorTask(void*) {
  for (;;) {
    sensors_tick();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void controlTask(void*) {
  esp_task_wdt_add(nullptr);

  // Boot self-test
  uint32_t fail = selftest_run();
  Event ev{}; ev.type = EV_SELFTEST_RESULT; ev.p.u32 = fail;
  sendEvent(ev);
  if (fail) {
    FaultEntry fe{}; fe.ts = millis(); fe.state = ST_BOOT_SELFTEST;
    fe.code = FC_SELFTEST_FAIL; fe.severity = SEV_WARN;
    faultlog_record(fe);
  }

  Event e{};
  for (;;) {
    if (receiveEvent(e, 50)) sm_handleEvent(e);
    link_tick();    // v6: detect tank-node link up/down, emit EV_LINK_*
    sm_tick();
    actuator_tick();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void safetyTask(void*) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    safety_tick();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============== CORE 0 ==============
static void buttonTask(void*) {
  for (;;) { buttons_tick(); vTaskDelay(pdMS_TO_TICKS(10)); }
}
static void uiTask(void*) {
  for (;;) { ui_tick(); vTaskDelay(pdMS_TO_TICKS(100)); }
}
static void ledTask(void*) {
  for (;;) { led_tick(); vTaskDelay(pdMS_TO_TICKS(50)); }
}
static void buzzerTask(void*) {
  for (;;) { buzzer_autoTick(); buzzer_tick(); vTaskDelay(pdMS_TO_TICKS(20)); }
}
static void mqttTask(void*) {
  for (;;) { mqtt_tick(); vTaskDelay(pdMS_TO_TICKS(50)); }
}

void initTasks() {
  // Watchdog (8 s) — only Control & Safety registered
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms     = (uint32_t)(TASK_WDT_TIMEOUT_S * 1000),
    .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
    .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdt_cfg);   // core auto-inits WDT; reconfigure is safe
#else
  esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);
#endif

  // ---- Core 1 (real-time) ----
  xTaskCreatePinnedToCore(sensorTask,  "Sensor",  STK_SENSOR,  nullptr, 3, &hSensor,  1);
  xTaskCreatePinnedToCore(controlTask, "Control", STK_CONTROL, nullptr, 4, &hControl, 1);
  xTaskCreatePinnedToCore(safetyTask,  "Safety",  STK_SAFETY,  nullptr, 5, &hSafety,  1);

  // ---- Core 0 (best-effort) ----
  xTaskCreatePinnedToCore(buttonTask,  "Button",  STK_BUTTON,  nullptr, 2, &hButton, 0);
  xTaskCreatePinnedToCore(uiTask,      "UI",      STK_UI,      nullptr, 1, &hUI,     0);
  xTaskCreatePinnedToCore(ledTask,     "LED",     STK_LED,     nullptr, 1, &hLED,    0);
  xTaskCreatePinnedToCore(buzzerTask,  "Buzz",    STK_BUZZER,  nullptr, 1, &hBuzz,   0);
  xTaskCreatePinnedToCore(mqttTask,    "MQTT",    STK_MQTT,    nullptr, 2, &hMQTT,   0);
}
