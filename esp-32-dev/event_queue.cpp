#include "event_queue.h"
#include "config.h"

static QueueHandle_t s_q = nullptr;

void initEventQueue() {
  s_q = xQueueCreate(EVENT_QUEUE_LEN, sizeof(Event));
  configASSERT(s_q != nullptr);
}

bool sendEvent(const Event& e, uint32_t timeoutMs) {
  if (!s_q) return false;
  return xQueueSend(s_q, &e, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool sendEventFromISR(const Event& e, BaseType_t* hpw) {
  if (!s_q) return false;
  return xQueueSendFromISR(s_q, &e, hpw) == pdTRUE;
}

bool receiveEvent(Event& out, uint32_t timeoutMs) {
  if (!s_q) return false;
  return xQueueReceive(s_q, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t queueDepth() {
  return s_q ? uxQueueMessagesWaiting(s_q) : 0;
}
