#include "fault_log.h"
#include <Preferences.h>

#define NS  "swtc_flt"

static Preferences s_p;
static FaultEntry  s_buf[FAULT_LOG_SIZE];
static uint16_t    s_head = 0;   // next write idx
static uint16_t    s_count = 0;
static uint32_t    s_seq = 0;    // RAM-only, so a reboot never replays old faults

// Index must track FaultCode in events.h.
static const char* const kCodeNames[] = {
  "NONE", "DRY_RUN", "NO_CURRENT", "NO_FEEDBACK", "OVERRUN", "OVERFLOW",
  "STUCK_PUMP", "IMPLAUSIBLE_LEVEL", "SUCTION_LOW", "SELFTEST_FAIL",
  "WDT_RESET", "BROWNOUT", "MQTT_DISCONNECT", "INTERLOCK_VIOLATION",
};
static const char* const kSevNames[] = { "INFO", "WARN", "ERROR", "PANIC" };

const char* faultCodeName(uint8_t code) {
  return (code < (sizeof(kCodeNames) / sizeof(kCodeNames[0]))) ? kCodeNames[code] : "UNKNOWN";
}

const char* faultSevName(uint8_t sev) {
  return (sev < (sizeof(kSevNames) / sizeof(kSevNames[0]))) ? kSevNames[sev] : "?";
}

uint32_t faultlog_seq() { return s_seq; }

static void persist() {
  s_p.putBytes("buf",  s_buf, sizeof(s_buf));
  s_p.putUShort("head", s_head);
  s_p.putUShort("cnt",  s_count);
}

void faultlog_init() {
  s_p.begin(NS, false);
  size_t got = s_p.getBytes("buf", s_buf, sizeof(s_buf));
  if (got != sizeof(s_buf)) {
    memset(s_buf, 0, sizeof(s_buf));
    s_head = 0; s_count = 0;
    persist();
  } else {
    s_head  = s_p.getUShort("head", 0);
    s_count = s_p.getUShort("cnt", 0);
  }
  Serial.printf("[FLT] log loaded (%u entries)\n", s_count);
}

void faultlog_record(const FaultEntry& e) {
  s_buf[s_head] = e;
  s_head = (s_head + 1) % FAULT_LOG_SIZE;
  if (s_count < FAULT_LOG_SIZE) s_count++;
  s_seq++;
  persist();
  Serial.printf("[FLT] %s (%s) state=%u level=%u%% flow=%u.%u I=%umV\n",
    faultCodeName(e.code), faultSevName(e.severity), e.state, e.levelPct,
    e.flowLpmX10 / 10, e.flowLpmX10 % 10, e.currentMv);
}

void faultlog_clear() {
  memset(s_buf, 0, sizeof(s_buf));
  s_head = 0; s_count = 0; s_seq = 0;
  persist();
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
