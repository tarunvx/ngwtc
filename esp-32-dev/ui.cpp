#include "ui.h"
#include "config.h"
#include "pins.h"
#include "sensors.h"
#include "settings.h"
#include "state_machine.h"
#include "time_utils.h"
#include "mqtt.h"
#include "menu.h"

#if HAS_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  #include <WiFi.h>
  static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#endif

// ---- Event-driven refresh ----
static uint32_t s_lastUiUpdate = 0;
static bool     s_uiDirty = true;
// Popup overlay
static char     s_popup[32] = {0};
static uint32_t s_popupUntil = 0;
// Operating screen toggle
static bool     s_opScreen = false;       // auto-enters when pump runs
static bool     s_opScreenForced = false; // user forced home while running

void ui_requestUpdate() { s_uiDirty = true; }

void ui_toggleOpScreen() {
  s_opScreenForced = !s_opScreenForced;
  s_uiDirty = true;
}

bool ui_isOpScreen() { return s_opScreen && !s_opScreenForced; }

void ui_showPopup(const char* msg, uint16_t durationMs) {
  strncpy(s_popup, msg, sizeof(s_popup)-1);
  s_popup[sizeof(s_popup)-1] = 0;
  s_popupUntil = millis() + durationMs;
  s_uiDirty = true;
}

void ui_init() {
#if HAS_OLED
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.printf("%s OLED init fail\n", LOG_TAG_UI);
    return;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 0);
  oled.println("Tarun");
  oled.println("   Labs");
  oled.display();
  vTaskDelay(pdMS_TO_TICKS(2000));
#endif
}

// Draw a simple WiFi/network symbol at (x,y)
static void drawWifi(int x, int y, bool on) {
#if HAS_OLED
  if (on) {
    // 3 arcs + dot
    oled.drawPixel(x+3, y+6, WHITE);
    oled.drawCircle(x+3, y+4, 2, WHITE);
    oled.drawCircle(x+3, y+2, 4, WHITE);
    oled.drawCircle(x+3, y,   6, WHITE);
  } else {
    // small X for disconnected
    oled.drawLine(x, y, x+6, y+6, WHITE);
    oled.drawLine(x+6, y, x, y+6, WHITE);
  }
#endif
}

void ui_tick() {
#if HAS_OLED
  // Popup expiry forces redraw
  if (s_popupUntil && millis() >= s_popupUntil) {
    s_popup[0] = 0;
    s_popupUntil = 0;
    s_uiDirty = true;
  }

  // Refresh on events or periodically (2s during pump, 5s idle)
  uint32_t now = millis();
  uint32_t refreshInterval = (sm_isPumpRunningState(sm_state()) || sm_state() == ST_STARTING) ? 2000 : 5000;
  if (!s_uiDirty && (now - s_lastUiUpdate < refreshInterval)) return;
  s_lastUiUpdate = now;
  s_uiDirty = false;

  oled.clearDisplay();

  // ---- Popup overlay ----
  if (s_popup[0]) {
    oled.setTextSize(2);
    oled.setTextColor(WHITE);
    // Center popup on screen
    int16_t x1, y1; uint16_t tw, th;
    oled.getTextBounds(s_popup, 0, 0, &x1, &y1, &tw, &th);
    int px = (128 - (int)tw) / 2; if (px < 0) px = 0;
    int py = (64 - (int)th) / 2;  if (py < 0) py = 0;
    oled.fillRect(px-4, py-4, tw+8, th+8, BLACK);
    oled.drawRect(px-4, py-4, tw+8, th+8, WHITE);
    oled.setCursor(px, py);
    oled.print(s_popup);
    oled.display();
    return;
  }

  // ---- Menu view (font 1 for more items) ----
  if (menu_isOpen() || menu_isEditing()) {
    // --- Value editor screen ---
    if (menu_isEditing()) {
      // Row 0: title (font 1)
      oled.setTextSize(1);
      oled.setCursor(0, 0);
      oled.print(menu_editTitle());
      // Row 1-2: value (font 2, large)
      oled.setTextSize(2);
      oled.setCursor(10, 20);
      oled.printf("%ld %s", (long)menu_editValue(), menu_editUnit());
      // Row 3: hints (font 1)
      oled.setTextSize(1);
      oled.setCursor(0, 48);
      oled.print("B3:+ B2:- B4:OK B1:X");
      oled.display();
      return;
    }
    // --- Normal menu list ---
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("== MENU ==");
    uint8_t sel = menu_selectedIndex();
    uint8_t cnt = menu_itemCount();
    int8_t start = (int8_t)sel - 2;
    if (start < 0) start = 0;
    if (start + 6 > cnt) start = (cnt > 6) ? cnt - 6 : 0;
    for (uint8_t i = 0; i < 6 && (start + i) < cnt; i++) {
      uint8_t idx = start + i;
      oled.printf("%c %s\n", idx == sel ? '>' : ' ', menu_itemLabel(idx));
    }
    oled.display();
    return;
  }

  // ---- Operating screen (auto-enters when pump is running) ----
  SystemState curSt = sm_state();
  bool pumpActive = sm_isPumpRunningState(curSt) || curSt == ST_STARTING || curSt == ST_STOPPING;
  s_opScreen = pumpActive;

  // Reset forced-home flag when pump stops
  if (!pumpActive) s_opScreenForced = false;

  if (pumpActive && !s_opScreenForced) {
    // Operating screen: big font, essential info only
    uint32_t runtime = sinceMs(sm_pumpStartedAt());
    uint32_t rSec = runtime / 1000;
    uint32_t rMin = rSec / 60;
    rSec %= 60;

    // Row 0 (y=0): STATE + runtime (font 2)
    oled.setTextSize(2);
    oled.setTextColor(WHITE);
    oled.setCursor(0, 0);
    const char* sn = sm_stateName(curSt);
    char sn5[6]; strncpy(sn5, sn, 5); sn5[5] = 0;
    oled.print(sn5);
    // Runtime at right
    char rtBuf[8];
    snprintf(rtBuf, sizeof(rtBuf), "%lu:%02lu", (unsigned long)rMin, (unsigned long)rSec);
    oled.setCursor(128 - (strlen(rtBuf) * 12), 0);
    oled.print(rtBuf);

    // Row 1 (y=18): Level (big)
    oled.setTextSize(2);
    oled.setCursor(0, 18);
    oled.printf("LVL: %3u%%", sensors_levelPct());

    // Row 2 (y=36): flow + current — same columns as the dashboard
    oled.setCursor(0, 36);
    uint16_t fl = (uint16_t)((sensors_flowLpmX10() + 5) / 10);
    uint16_t ia = sensors_currentAmps();
    if (fl > 99) fl = 99;
    if (ia > 99) ia = 99;
    oled.printf("FL:%2u", fl);
    oled.setCursor(68, 36);
    oled.printf("I:%2u", ia);

    // Row 3 (y=54): overflow-run / bypass warnings (font 1)
    oled.setTextSize(1);
    oled.setCursor(0, 56);
    bool bI = settings().bypassCurrentSense;
    bool bF = settings().bypassFlowSense;
    if (sm_overflowIgnore()) oled.print("** OVERFLOW RUN 1x **");
    else if (bI && bF)      oled.print("!! I+F BYPASSED !!");
    else if (bI)       oled.print("!! I-SENSE BYPASS !!");
    else if (bF)       oled.print("!! FLOW BYPASS !!");
    else               oled.print("B1:Home  B3L:STOP");

    oled.display();
    return;
  }

  // ---- Dashboard (font 2, no headings) ----
  // Font 2: each char = 12w × 16h, screen = 128×64 → ~10 chars × 4 rows
  // Row 5 at y=56 uses font 1 (8px) for day + time

  // Row 1 (y=0): level%   state   wifi
  oled.setTextSize(2);
  oled.setTextColor(WHITE);
  oled.setCursor(0, 0);
  oled.printf("%3u%%", sensors_levelPct());
  // State name (truncate to 4 chars for fit)
  const char* sn = sm_stateName(sm_state());
  char sn4[5]; strncpy(sn4, sn, 4); sn4[4] = 0;
  oled.setCursor(52, 0);
  oled.print(sn4);
  // WiFi symbol at top-right
  drawWifi(118, 0, WiFi.isConnected());

  // Row 2 (y=16): pressure   mode
  oled.setCursor(0, 16);
  float pres = sensors_pressureMPa();
  char pbuf[11];
  dtostrf(pres, 4, 2, pbuf);
  oled.printf("%sMPa", pbuf);
  // Mode at right
  const char* mn = modeName(settings().mode);
  char mn4[5]; strncpy(mn4, mn, 4); mn4[4] = 0;
  oled.setCursor(92, 16);
  oled.print(mn4);

  // Row 3 (y=32): flow (whole L/min) + current (whole amps)
  oled.setCursor(0, 32);
  uint16_t fl  = (uint16_t)((sensors_flowLpmX10() + 5) / 10);
  uint16_t ia  = sensors_currentAmps();
  if (fl > 99) fl = 99;
  if (ia > 99) ia = 99;
  oled.printf("FL:%2u", fl);
  oled.setCursor(68, 32);
  oled.printf("I:%2u", ia);

  // Row 4 (y=48): temp + humidity (font 1 to fit both)
  oled.setTextSize(1);
  oled.setCursor(0, 48);
  int16_t tc = sensors_tempCx10();
  uint16_t rh = sensors_rhX10();
  if (tc > -9990) {
    oled.printf("T:%d.%dC  RH:%u%%", tc/10, abs(tc%10), rh/10);
  } else {
    oled.printf("T:--.-C  RH:--%% ");
  }

  // Row 5 (y=56): overflow-armed banner (when armed) else day + time (NTP)
  oled.setCursor(0, 56);
  if (sm_overflowIgnore()) {
    oled.print("** OVERFLOW ARMED 1x");
  } else {
    time_t tnow = time(nullptr);
    struct tm tmnow;
    localtime_r(&tnow, &tmnow);
    if (tnow > 100000) {  // NTP synced (not epoch 0)
      char tbuf[20];
      strftime(tbuf, sizeof(tbuf), "%a %I:%M %p", &tmnow);
      oled.print(tbuf);
    } else {
      // Fallback: show uptime
      uint32_t up = millis()/1000;
      oled.printf("UP %luh%02lum", up/3600, (up/60)%60);
    }
  }

  oled.display();
#endif
}
