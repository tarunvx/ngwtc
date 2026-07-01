#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include "events.h"

void initEventQueue();
bool sendEvent(const Event& e, uint32_t timeoutMs = 0);
bool sendEventFromISR(const Event& e, BaseType_t* hpw);
bool receiveEvent(Event& out, uint32_t timeoutMs);
uint32_t queueDepth();

#endif
