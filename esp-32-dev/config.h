#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  Smart Water Tank Controller — central configuration
//  Flip feature flags as hardware / libraries come online.
// ============================================================

// Single source of truth for this tree's version. Bump the MINOR digit on
// every change; MAJOR only for a redesign. Reported by GET:DIAG as fw=.
#define FIRMWARE_VERSION  "5.4.0"

// ---- Feature flags (compile-time) --------------------------
#define HAS_OLED        1   // SSD1306 via I2C
#define HAS_NEOPIXEL    1   // WS2812B x8
#define HAS_DHT22       1   // ambient sensor
#define HAS_MQTT        1   // Adafruit MQTT + WiFi
#define HAS_FLOW_ISR    1   // YF-S201
#define HAS_CT_CLAMP    0   // SCT-013 via ADC
#define OTA_ENABLED     0   // partition reserved; enable later

// ---- Runtime feature toggles (overridable via NVS settings) ----
#define DEFAULT_HW_TIMER_PRESENT   false   // 555/ATtiny upper-bound timer
#define DEFAULT_ADAPTIVE_DRYRUN    false   // experimental
#define DEFAULT_MCU_UPS_PRESENT    false

// ---- Phase-1 / bring-up flag --------------------------------
//  When 1: AUTO logic disabled, actuator pulses suppressed, only MANUAL
//  pump ON/OFF requests are honored (and only if you also enable below).
//  Useful for safely watching sensors while hardware is being assembled.
#define MONITOR_ONLY_MODE      1
//  Even in MONITOR_ONLY_MODE, allow MANUAL pump pulses? (set 0 to fully
//  disable any solenoid pulse from firmware).
#define ALLOW_MANUAL_ACTUATION 1

// ---- Level source -----------------------------------------
enum LevelSource {
  LVL_FLOAT = 0,
  LVL_ULTRASONIC = 1,
  LVL_PRESSURE = 2
};
#define DEFAULT_LEVEL_SOURCE  LVL_FLOAT

// ---- Default tunables (ms) — all overridable in NVS -------
#define DEF_PULSE_ON_MS         1000
#define DEF_PULSE_OFF_MS        1000
#define DEF_DRYRUN_MS           15000
#define DEF_FEEDBACK_MS         1500
#define DEF_CURRENT_MS          3000
#define DEF_CURRENT_OFF_MS      4000
#define DEF_FLOW_OFF_MS         8000
#define DEF_MAX_RUNTIME_MS      1800000UL   // 30 min
#define DEF_TIMER1_MS           300000UL    // 5 min
#define DEF_TIMER2_MS           600000UL    // 10 min
#define DEF_OVERRUN_COOLDOWN_MS 60000UL
#define DEF_FAULT_REPEAT_WINDOW 600000UL    // 10 min
#define DEF_FAULT_REPEAT_LIMIT  3

// ---- Sensor thresholds ------------------------------------
#define DEF_FLOW_KPPL           450     // YF-S201 pulses per litre (F=7.5·Q ⇒ 450)
#define DEF_MIN_LPM_X10         5       // legacy floor; runtime "flow present" cutoff
                                        // is now NVS flowNoFlowThresh via
                                        // sensors_flowThreshX10() (this is only a default seed)
#define DEF_CT_OFFSET_MV        1650
#define DEF_CT_THRESH_MV        80      // RMS over baseline
// CT sampling window: 40 ms = 2 full mains cycles at 50 Hz.
#define CT_SAMPLE_MS            40
// Display scaling for the CT clamp: SCT-013-030 is 30 A : 1 V => 0.030 A/mV.
#define CT_AMPS_PER_MV_X1000    30
#define DEF_LEVEL_HYST          3       // %

// ---- Queue / task sizing ----------------------------------
#define EVENT_QUEUE_LEN         32
#define STK_SENSOR              4096
#define STK_CONTROL             6144
#define STK_SAFETY              4096
#define STK_BUTTON              2048
#define STK_UI                  4096
#define STK_LED                 2048
#define STK_BUZZER              2048
#define STK_MQTT                6144
#define STK_DHT                 2048

// ---- Watchdog ---------------------------------------------
#define TASK_WDT_TIMEOUT_S      8

// ---- MQTT (Adafruit) — credentials in SECRETS.h ------------
#include "SECRETS.h"
#define MQTT_PUBLISH_PERIOD_MS  30000UL

// ---- Logging ----------------------------------------------
#define LOG_TAG_SYS  "[SYS]"
#define LOG_TAG_SM   "[SM]"
#define LOG_TAG_ACT  "[ACT]"
#define LOG_TAG_SAF  "[SAF]"
#define LOG_TAG_SEN  "[SEN]"
#define LOG_TAG_MQ   "[MQ]"
#define LOG_TAG_UI   "[UI]"

#endif
