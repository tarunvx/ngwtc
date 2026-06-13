---
Two changes made:

NEO_COUNT reduced to 9 in pins.h (removed the dead LED)

Fill direction reversed — now fills from top (high index) downward (low index), matching your inverted physical mounting:

0% → top 2 LEDs (index 8, 7) blink red
25% → top 3 LEDs orange
50% → top 5 LEDs yellow
75% → top 7 LEDs blue
100% → all 9 LEDs green
---
relay modules are active-LOW (common for opto-isolated modules). Currently both pins sit LOW in idle, which keeps relay coils energized and draws current continuously. We need to reverse the logic:

Idle: both pins HIGH → relays de-energized → no current draw
Pulse: drive pin LOW → energizes relay briefly → switches solenoid
Interlock: both LOW simultaneously = both relays energized = fault
actuator.cpp+14-13
Done. The relay logic is now reversed for active-LOW relay modules:

State	PIN_RELAY_ON	PIN_RELAY_OFF	Effect
Idle	HIGH	HIGH	Both coils off — zero current draw
Pulse ON	LOW	HIGH	ON relay energized, OFF relay safe
Pulse OFF	HIGH	LOW	OFF relay energized, ON relay safe
Interlock fault	LOW	LOW	Both energized = panic → force both HIGH
The relays now only draw current during the brief pulse window (default 1 second), then return to idle (HIGH = coil off).
---
 The startup animation in led_init() now runs a rainbow chase sequence:

Chase phase (~1s): Each LED lights up one-by-one with a 2-LED fading trail, colors spread across the rainbow (red → orange → yellow → green → cyan → blue → violet)
Hold phase (0.4s): Full rainbow displayed across all 10 LEDs
Fade-out (~0.2s): All LEDs dim to black smoothly
Total duration: ~1.6 seconds. After that, normal level-based display takes over.
---
Fixes
LED Array Level Display
Problem: The old code lit LEDs from the TOP down (index NEO_COUNT-N to NEO_COUNT), and at 0% only lit a single LED — looked broken.

Fix: Rewrote to fill from the BOTTOM up (index 0 upward):

0% → 2 LEDs red blinking (empty warning, very visible)
<25% → 2 LEDs red solid
25-49% → 3 LEDs orange
50-74% → 5 LEDs yellow
75-99% → 7 LEDs blue
100% → All 10 LEDs green
Temp/Humidity (DHT22)
Problem: DHT22 timing-sensitive reads fail under FreeRTOS preemption. Single failed read → values stay at sentinel (-9999), showing T:--.-C.

Fix: Added 3-attempt retry loop per read cycle, increased interval to 2.5s for better sensor recovery, and added failure logging to Serial.
---
Fixes Applied
1. Flow Sensor — Reverted Pump Gating
Since you have a shared water line with another system, the flow sensor must report real readings even when YOUR pump is off. Removed the pump-state gate. The averaging (4 samples) + increased threshold (2.0 L/min) will filter cable noise while showing real flow from either system.

2. Feedback Bypass (bypassFeedback)
Added new bypassFeedback setting:

Menu item: "Bypass Feedback" (toggles ON/OFF)
MQTT: CMD:x:SET:bypassFeedback:1
When ON: STARTING skips waiting for feedback switch, STOPPING doesn't fault on missing feedback
Combined with I+F bypass, the system goes: STARTING → (all bypassed = immediate pass) → MANUAL_ON
3. PUMP:ON → SLEEP Bug Fixed
Root cause: sleepMode was persisted in NVS from a previous session. The IDLE tick (sm_tick) checks sleepMode and pushes to SLEEP every 20ms. When PUMP:ON triggered, it entered STARTING but as soon as an error pushed it back to IDLE, it got shoved into SLEEP.

Fix:

B1_LONG, B2, B3 button handlers now clear sleepMode before entering STARTING
MQTT PUMP:ON now wakes the system from SLEEP instead of rejecting the command
4. Dedicated Level MQTT Topic
Water level (0-100 integer) now publishes to a separate topic: .../swtc-slash-level
Your other system can subscribe to this for cross-tank coordination.

5. Serial Debug
Added 500ms delay for USB-CDC enumeration (was 200ms, too short for some USB chipsets)
Added explicit Serial.flush() after boot banner
Boot message now shows baud rate reminder: === Baud: 115200 ===
Note: Since the Settings struct size changed (added bypassFeedback), NVS will auto-reset to defaults on first boot. sleepMode will be false, all bypass flags false. You'll need to re-enable your bypass flags via menu or MQTT after flashing.

LED Fix
The LED issue was a symptom, not a bug. Because the system was stuck in ERROR/SLEEP, the LED override patterns (red blink for ERROR, dim blue for SLEEP) were active instead of the level display. With the state machine now staying in IDLE properly, the level-based LED colors will show correctly.

------

Summary of Changes
1. Operating Screen (pump running)
File: ui.cpp, ui.h, state_machine.cpp

When pump is active (STARTING/RUNNING/STOPPING), OLED automatically switches to an operating screen showing:
State name + runtime (MM:SS) in big font
Water level %
Flow (L/min) + Current (mV)
Bypass warnings if any sensor is bypassed
B1 short-press toggles between operating screen and home dashboard while pump runs
Refresh rate is 2s during pump operation (vs 5s idle)
2. Bypass CurrentSense / FlowSense
Files: settings.h, settings.cpp, menu.cpp, state_machine.cpp

New settings: bypassCurrentSense, bypassFlowSense (bool, default false)
Menu items "Bypass I-Sense" and "Bypass Flow" toggle these on/off
When bypassed:
STARTING state skips waiting for current/flow confirmation
Running states won't fault on current loss
STOPPING state won't panic on residual readings
Warning shown on operating screen: !! I+F BYPASSED !!
Settable via MQTT: CMD:x:SET:bypassCurrentSense:1
3. Flow Sensor Smoothing & Threshold
Files: sensors.cpp, settings.h, settings.cpp, menu.cpp

Moving average (configurable 1–8 samples, default 4) smooths flow readings
No-flow threshold (flowNoFlowThresh, default 5 = 0.5 L/min): readings below this are reported as 0
Output shown as clean integer X.Y L/min (no garbage/fluctuation)
Both values are settable via menu ("Flow Threshold") and MQTT
4. Remote Actuation Verification
File: mqtt.cpp

CMD:x:PUMP:ON → switches to MANUAL, triggers STARTING (respects bypass flags)
CMD:x:PUMP:ON:TIMER:10 → sets timer to 10 min, triggers timer mode
CMD:x:PUMP:OFF → hard stop (only if pump is actually running, else ACKs "NOT_RUNNING")
Added ALREADY_RUNNING guard to prevent duplicate starts
Bypass flags respected throughout the actuation chain
MQTT status JSON now includes bypass_i and bypass_f flags

-----