#include "fault_log.h"
#include <Preferences.h>

#define NS  "swtc_flt"

static Preferences s_p;
static FaultEntry  s_buf[FAULT_LOG_SIZE];
static uint16_t    s_head = 0;   // next write idx
static uint16_t    s_count = 0;

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
  persist();
  Serial.printf("[FLT] code=%u sev=%u state=%u level=%u%%\n",
    e.code, e.severity, e.state, e.levelPct);
}

void faultlog_clear() {
  memset(s_buf, 0, sizeof(s_buf));
  s_head = 0; s_count = 0;
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
      s.printf("[%lu ms] state=%u code=%u sev=%u lvl=%u%% flow=%u.%u lpm I=%u mV\n",
        (unsigned long)e.ts, e.state, e.code, e.severity,
        e.levelPct, e.flowLpmX10/10, e.flowLpmX10%10, e.currentMv);
    }
  }
}
