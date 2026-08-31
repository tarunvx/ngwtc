#ifndef TASKS_H
#define TASKS_H
void initTasks();

// Stops every task except the caller (MQTT) and unregisters the watchdog, so a
// long run of flash writes cannot trip it. Resume is only for a failed update;
// a successful one reboots.
void tasks_prepareForOta();
void tasks_resumeAfterOta();
#endif
