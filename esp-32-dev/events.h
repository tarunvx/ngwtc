#ifndef EVENTS_H
#define EVENTS_H

#include <Arduino.h>

// Single source of truth for events.

enum EventType : uint8_t {
  EV_NONE = 0,

  // Sensor
  EV_LEVEL_CHANGED,        // payload.i32 = percent (0..100)
  EV_FLOW_TICK,            // payload.flow {lpm_x10, totalLx10}
  EV_CURRENT_PRESENT,      // payload.boolean
  EV_FB_ON,                // payload.boolean (pressed)
  EV_FB_OFF,               // payload.boolean
  EV_DHT_READ,             // payload.dht {tempCx10, rhx10}

  // User
  EV_BUTTON,               // payload.btn {id, kind}

  // Timers
  EV_TIMER_EXPIRED,        // payload.i32 = which (1,2,override,maxRuntime)

  // External
  EV_MQTT_CMD,             // payload.cmd {id, domain, action, args[2]}

  // Faults
  EV_FAULT,                // payload.fault {code, severity}
  EV_PANIC_STOP,           // payload.i32 = reason code

  // Self-test
  EV_SELFTEST_RESULT,      // payload.u32 = pass mask (0 = all pass)

  // Mode change request from any source
  EV_MODE_REQ,             // payload.i32 = Mode enum
};

enum ButtonId   : uint8_t { BTN1 = 1, BTN2 = 2, BTN3 = 3, BTN4 = 4 };
enum PressKind  : uint8_t { PRESS_SHORT = 0, PRESS_LONG = 1 };
enum TimerId    : uint8_t { TMR_1 = 1, TMR_2 = 2, TMR_OVERRIDE = 3, TMR_MAX_RUNTIME = 4 };

enum FaultCode : uint8_t {
  FC_NONE = 0,
  FC_DRY_RUN,
  FC_NO_CURRENT,
  FC_NO_FEEDBACK,
  FC_OVERRUN,
  FC_OVERFLOW,
  FC_STUCK_PUMP,
  FC_IMPLAUSIBLE_LEVEL,
  FC_SUCTION_LOW,
  FC_SELFTEST_FAIL,
  FC_WDT_RESET,
  FC_BROWNOUT,
  FC_MQTT_DISCONNECT,
  FC_INTERLOCK_VIOLATION,
};

enum FaultSeverity : uint8_t { SEV_INFO = 0, SEV_WARN = 1, SEV_ERROR = 2, SEV_PANIC = 3 };

enum CmdDomain : uint8_t { CD_PUMP, CD_MODE, CD_SET, CD_GET, CD_SYS };

struct CmdPayload {
  uint16_t   id;        // CMD id for ACK
  CmdDomain  domain;
  uint8_t    action;    // domain-specific
  int32_t    arg0;
  int32_t    arg1;
};

struct FlowPayload {
  uint16_t lpm_x10;
  uint32_t totalL_x10;
};

struct DhtPayload {
  int16_t  tempCx10;
  uint16_t rhx10;
};

struct BtnPayload {
  ButtonId  id;
  PressKind kind;
};

struct FaultPayload {
  FaultCode     code;
  FaultSeverity severity;
};

struct Event {
  EventType type;
  union {
    int32_t       i32;
    uint32_t      u32;
    bool          boolean;
    BtnPayload    btn;
    FlowPayload   flow;
    DhtPayload    dht;
    FaultPayload  fault;
    CmdPayload    cmd;
  } p;
};

#endif
