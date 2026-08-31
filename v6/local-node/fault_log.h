#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#include <Arduino.h>
#include "events.h"

#define FAULT_LOG_SIZE 32

struct FaultEntry {
  uint32_t      ts;          // millis at log time
  uint8_t       state;       // SystemState at time of fault
  FaultCode     code;
  FaultSeverity severity;
  uint8_t       levelPct;
  uint16_t      flowLpmX10;
  uint16_t      currentMv;
};

void  faultlog_init();
void  faultlog_record(const FaultEntry& e);
void  faultlog_clear();
void  faultlog_dump(Stream& s);
size_t faultlog_count();
bool  faultlog_get(size_t idx, FaultEntry& out);

// Monotonic since boot (0 at reset, also reset by faultlog_clear) so consumers
// such as MQTT can spot new entries without re-scanning the ring.
uint32_t faultlog_seq();

// Commits any deferred entries. Call periodically from a non-critical task.
void faultlog_flush();
uint32_t faultlog_suppressed();

// Logs why the last boot happened and records abnormal causes (brownout,
// watchdog, panic) so they reach MQTT. Call once, after faultlog_init().
void faultlog_recordBootReason();
const char* faultlog_resetReasonName();

const char* faultCodeName(uint8_t code);
const char* faultSevName(uint8_t sev);

#endif
