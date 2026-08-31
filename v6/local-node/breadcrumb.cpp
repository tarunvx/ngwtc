#include "breadcrumb.h"

#define BC_MAGIC 0xB1C0FFEEUL

RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint8_t  s_phase[2];
RTC_NOINIT_ATTR static uint32_t s_ts[2];

static char s_report[48] = "n/a";

static const char* const BC_NAMES[BC_COUNT] = {
  "NONE", "SETUP", "STEST", "SENS", "LINK", "SM", "ACT",
  "SAFE", "BTN", "UI", "LED", "BUZZ", "MQTT", "FLUSH", "NVS", "IDLE"
};

static const char* nameOf(uint8_t p) {
  return (p < BC_COUNT) ? BC_NAMES[p] : "?";
}

// A core that never marked (ts still 0) must not be reported as a huge age.
static void fmtCore(char* out, size_t n, uint8_t core, uint32_t newest) {
  if (s_phase[core] == BC_NONE && s_ts[core] == 0) { snprintf(out, n, "-"); return; }
  snprintf(out, n, "%s+%lu", nameOf(s_phase[core]),
           (unsigned long)(newest - s_ts[core]));
}

void bc_captureBoot() {
  if (s_magic == BC_MAGIC) {
    uint32_t newest = (s_ts[0] > s_ts[1]) ? s_ts[0] : s_ts[1];
    char a[20], b[20];
    fmtCore(a, sizeof(a), 0, newest);
    fmtCore(b, sizeof(b), 1, newest);
    snprintf(s_report, sizeof(s_report), "c0:%s c1:%s", a, b);
  }
  s_magic = BC_MAGIC;
  s_phase[0] = s_phase[1] = BC_NONE;
  s_ts[0] = s_ts[1] = 0;
}

void bc_mark(uint8_t phase) {
  uint32_t c = xPortGetCoreID();
  s_phase[c] = phase;
  s_ts[c]    = millis();
}

const char* bc_report() { return s_report; }
