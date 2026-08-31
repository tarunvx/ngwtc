#include "fault_log.h"
#include <Preferences.h>
#include <esp_system.h>

#define NS  "swtc_flt"

static Preferences s_p;
static FaultEntry  s_buf[FAULT_LOG_SIZE];
static uint16_t    s_head = 0;   // next write idx
static uint16_t    s_count = 0;
static uint32_t    s_seq = 0;    // RAM-only, so a reboot never replays old faults
static bool        s_dirty = false;
static FaultCode   s_lastCode = FC_NONE;
static uint32_t    s_lastTs = 0;
static uint32_t    s_suppressed = 0;

#define FAULT_DEDUP_MS 60000UL   // same code within this window is counted, not stored

// Index must track FaultCode in events.h.
static const char* const kCodeNames[] = {
  "NONE", "DRY_RUN", "NO_CURRENT", "NO_FEEDBACK", "OVERRUN", "OVERFLOW",
  "STUCK_PUMP", "IMPLAUSIBLE_LEVEL", "SUCTION_LOW", "SELFTEST_FAIL",
  "WDT_RESET", "BROWNOUT", "MQTT_DISCONNECT", "INTERLOCK_VIOLATION",
  "LINK_LOST", "PANIC",
};
static const char* const kSevNames[] = { "INFO", "WARN", "ERROR", "PANIC" };

const char* faultCodeName(uint8_t code) {
  return (code < (sizeof(kCodeNames) / sizeof(kCodeNames[0]))) ? kCodeNames[code] : "UNKNOWN";
}

const char* faultSevName(uint8_t sev) {
  return (sev < (sizeof(kSevNames) / sizeof(kSevNames[0]))) ? kSevNames[sev] : "?";
}

uint32_t faultlog_seq() { return s_seq; }

static const char* s_resetName = "?";

const char* faultlog_resetReasonName() { return s_resetName; }

void faultlog_recordBootReason() {
  FaultCode code = FC_NONE;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   s_resetName = "POWERON";                            break;
    case ESP_RST_EXT:       s_resetName = "EXT";                                break;
    case ESP_RST_SW:        s_resetName = "SW";                                 break;
    case ESP_RST_DEEPSLEEP: s_resetName = "DEEPSLEEP";                          break;
    case ESP_RST_PANIC:     s_resetName = "PANIC";     code = FC_PANIC;         break;
    case ESP_RST_INT_WDT:   s_resetName = "INT_WDT";   code = FC_WDT_RESET;     break;
    case ESP_RST_TASK_WDT:  s_resetName = "TASK_WDT";  code = FC_WDT_RESET;     break;
    case ESP_RST_WDT:       s_resetName = "OTHER_WDT"; code = FC_WDT_RESET;     break;
    case ESP_RST_BROWNOUT:  s_resetName = "BROWNOUT";  code = FC_BROWNOUT;      break;
    default:                s_resetName = "UNKNOWN";                            break;
  }
  Serial.printf("[BOOT] reset=%s heap=%lu minheap=%lu\n", s_resetName,
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());

  if (code != FC_NONE) {
    FaultEntry e{};
    e.ts = millis(); e.state = 0; e.code = code; e.severity = SEV_WARN;
    faultlog_record(e);
  }
}

// Only the most recent entry is persisted (12 B instead of 384 B). The ring
// lives in RAM and the full history goes to MQTT; all NVS has to answer is
// "what happened just before the reset?".
static void persist() {
  if (!s_count) return;
  uint16_t last = (uint16_t)((s_head + FAULT_LOG_SIZE - 1) % FAULT_LOG_SIZE);
  s_p.putBytes("last", &s_buf[last], sizeof(FaultEntry));
}

void faultlog_init() {
  s_p.begin(NS, false);

  // Drop the legacy whole-ring blob; rewriting it was the main NVS churn.
  s_p.remove("buf"); s_p.remove("head"); s_p.remove("cnt");

  memset(s_buf, 0, sizeof(s_buf));
  s_head = 0; s_count = 0;

  FaultEntry last{};
  if (s_p.getBytes("last", &last, sizeof(last)) == sizeof(last) && last.code != FC_NONE) {
    s_buf[0] = last;
    s_head = 1; s_count = 1;
    Serial.printf("[FLT] last fault before reboot: %s (%s) lvl=%u%%\n",
      faultCodeName(last.code), faultSevName(last.severity), last.levelPct);
  } else {
    Serial.println(F("[FLT] no stored fault"));
  }
}

void faultlog_record(const FaultEntry& e) {
  // Collapse repeats: a flapping link would otherwise fill the 32-slot ring in
  // seconds and bury the faults that actually explain a reboot.
  if (s_count && e.code == s_lastCode && (uint32_t)(e.ts - s_lastTs) < FAULT_DEDUP_MS) {
    s_suppressed++;
    return;
  }
  s_lastCode = e.code;
  s_lastTs   = e.ts;

  s_buf[s_head] = e;
  s_head = (s_head + 1) % FAULT_LOG_SIZE;
  if (s_count < FAULT_LOG_SIZE) s_count++;
  s_seq++;

  // Flash erase stalls the instruction cache on BOTH cores, which can overrun
  // the link UART buffer. Only commit immediately when the fault is serious
  // enough that losing it to a reset would matter.
  if (e.severity >= SEV_ERROR) persist();
  else                        s_dirty = true;

  Serial.printf("[FLT] %s (%s) state=%u level=%u%% flow=%u.%u I=%umV\n",
    faultCodeName(e.code), faultSevName(e.severity), e.state, e.levelPct,
    e.flowLpmX10 / 10, e.flowLpmX10 % 10, e.currentMv);
}

void faultlog_flush() {
  if (!s_dirty) return;
  s_dirty = false;
  persist();
}

uint32_t faultlog_suppressed() { return s_suppressed; }

void faultlog_clear() {
  memset(s_buf, 0, sizeof(s_buf));
  s_head = 0; s_count = 0; s_seq = 0;
  s_lastCode = FC_NONE; s_lastTs = 0; s_suppressed = 0; s_dirty = false;
  s_p.remove("last");
}

size_t faultlog_count() { return s_count; }

bool faultlog_get(size_t idx, FaultEntry& out) {
  if (idx >= s_count) return false;
  // oldest-first ordering
  uint16_t start = (s_count == FAULT_LOG_SIZE) ? s_head : 0;
  uint16_t real  = (start + idx) % FAULT_LOG_SIZE;
  out = s_buf[real];
  return true;
}

void faultlog_dump(Stream& s) {
  s.printf("== Fault Log (%u) ==\n", s_count);
  FaultEntry e;
  for (size_t i = 0; i < s_count; i++) {
    if (faultlog_get(i, e)) {
      s.printf("%2u) [%lu ms] %-18s %-5s state=%u lvl=%u%% flow=%u.%u lpm I=%u mV\n",
        (unsigned)(i + 1), (unsigned long)e.ts,
        faultCodeName(e.code), faultSevName(e.severity), e.state,
        e.levelPct, e.flowLpmX10/10, e.flowLpmX10%10, e.currentMv);
    }
  }
}
