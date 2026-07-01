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

#endif
