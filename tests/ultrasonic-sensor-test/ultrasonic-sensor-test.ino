/*
  ============================================================
   SWTC \u2014 JSN-SR04T ULTRASONIC DISTANCE TEST  (NodeMCU ESP-12E / ESP8266)
  ============================================================

   Sensor: REES52 JSN-SR04T waterproof ultrasonic transducer (DC 5V).
   Goal: interface it on a NodeMCU ESP-12E and print a STABLE, ACCURATE
   distance on the Serial Monitor.

   WHY EARLIER READINGS WERE "USELESS" (and how this sketch fixes it)
   ----------------------------------------------------------------
   The JSN-SR04T is notoriously noisy, and there are three classic traps.
   This sketch (plus the wiring below) addresses all three:

   1) ECHO is a 5V logic output \u2014 feeding it straight into a 3.3V ESP8266
      GPIO gives flaky/garbage readings (and stresses the pin). FIX: a simple
      resistor divider on ECHO (see wiring) drops 5V \u2192 ~3.3V.

   2) The module draws current spikes on each ping; on a weak 5V rail this
      browns out the transducer \u2192 wild readings. FIX: solder/attach a
      **100 \u00b5F electrolytic capacitor across the sensor's VCC and GND**
      (right at the module). This single change fixes most instability.

   3) Single pings are jumpy. FIX (software): take several samples, drop
      out-of-range ones, take the **median** (kills outlier spikes), then a
      light exponential smoothing (EMA) for a rock-steady number.

   ----------------------------------------------------------------
   WIRING  (NodeMCU ESP-12E)
   ----------------------------------------------------------------
     JSN-SR04T   NodeMCU
     ---------   -------
     5V (VCC) -> VIN (5V from USB)      + 100\u00b5F cap between VCC and GND
     GND      -> GND
     TRIG     -> D1 (GPIO5)             (3.3V trigger is fine for the sensor)
     ECHO     -> [ divider ] -> D2 (GPIO4)

   ECHO voltage divider (5V -> 3.3V):

        ECHO ---[ R1 = 1k ]---+--- D2 (GPIO4)
                              |
                          [ R2 = 2k ]
                              |
                             GND

     V_D2 = 5V * R2/(R1+R2) = 5 * 2/3 = 3.33V   \u2705
     (1k/2k shown; 2.2k/3.3k or 1k/1.8k also fine \u2014 aim for ~3.3V.)

   ----------------------------------------------------------------
   NOTES
   ----------------------------------------------------------------
   - Mode: assumes the default **Mode 0** (HC-SR04-compatible Trig/Echo).
     On JSN-SR04T v2.0/v3.0 this is the factory default (no R27 resistor
     populated). If yours was set to a UART/auto mode, remove that resistor.
   - Dead zone: the JSN-SR04T cannot measure closer than ~**20\u201325 cm** \u2014
     objects nearer than that read wrong. Usable range ~25 cm to ~600 cm.
   - For tank level: mount the transducer above the water pointing down;
     water_level = tank_height - measured_distance.

   Board: "NodeMCU 1.0 (ESP-12E Module)"  |  115200 baud
  ============================================================
*/

#include <Arduino.h>

// ---- Pins (NodeMCU labels) ---------------------------------
#define TRIG_PIN  D1    // GPIO5  -> sensor TRIG
#define ECHO_PIN  D2    // GPIO4  <- sensor ECHO (through 1k/2k divider)

// ---- Measurement tuning ------------------------------------
#define SAMPLES            11      // pings per reading (ODD -> clean median)
#define PING_INTERVAL_MS   60      // gap between pings (>=60ms lets echoes die)
#define ECHO_TIMEOUT_US    25000UL // pulseIn timeout (~4.3 m); raise for longer
#define MIN_VALID_CM       20.0f   // below the dead zone -> reject
#define MAX_VALID_CM       600.0f  // sensor max range -> reject
#define MIN_GOOD_SAMPLES   5       // need at least this many valid pings
#define EMA_ALPHA          0.30f   // 0..1 smoothing (lower = smoother/slower)
#define AIR_TEMP_C         25.0f   // ambient temp for speed-of-sound (accuracy)

// Speed of sound (temperature compensated): c = 331.4 + 0.606*T  [m/s]
// distance_cm = echo_us * (c/10000) / 2     (round trip -> divide by 2)
static float cmPerUs() { return (331.4f + 0.606f * AIR_TEMP_C) / 10000.0f; }

static bool  s_haveEma = false;
static float s_ema     = 0.0f;

// One ping. Returns distance in cm, or -1 on timeout / out of range.
static float pingOnceCm() {
  // Clean 10 us trigger pulse.
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo HIGH width (µs). 0 = timeout (no echo).
  unsigned long dur = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (dur == 0) return -1.0f;

  float cm = dur * cmPerUs() / 2.0f;
  if (cm < MIN_VALID_CM || cm > MAX_VALID_CM) return -1.0f;
  return cm;
}

// Insertion sort (tiny arrays) for the median.
static void sortAsc(float* a, int n) {
  for (int i = 1; i < n; i++) {
    float key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
}

// Median-filtered reading. Returns cm, or -1 if too few valid pings.
static float readDistanceCm(int* validOut, float* spreadOut) {
  float buf[SAMPLES];
  int   valid = 0;

  for (int i = 0; i < SAMPLES; i++) {
    float d = pingOnceCm();
    if (d > 0) buf[valid++] = d;
    delay(PING_INTERVAL_MS);
  }

  *validOut = valid;
  if (valid < MIN_GOOD_SAMPLES) { *spreadOut = 0; return -1.0f; }

  sortAsc(buf, valid);
  *spreadOut = buf[valid - 1] - buf[0];   // min..max spread (jitter indicator)
  return buf[valid / 2];                  // median
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("   JSN-SR04T Ultrasonic Test  (NodeMCU ESP-12E)"));
  Serial.println(F("=================================================="));
  Serial.printf ("  TRIG=D1(GPIO5)  ECHO=D2(GPIO4, via 1k/2k divider)\n");
  Serial.printf ("  samples/reading=%d  temp=%.1fC  range=%.0f..%.0f cm\n",
                 SAMPLES, AIR_TEMP_C, MIN_VALID_CM, MAX_VALID_CM);
  Serial.println(F("  TIP: add a 100uF cap across sensor VCC-GND for stability."));
  Serial.println(F("--------------------------------------------------"));

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  delay(50);
}

void loop() {
  int   valid;
  float spread;
  float median = readDistanceCm(&valid, &spread);

  if (median < 0) {
    s_haveEma = false;   // reset smoothing after a dropout
    Serial.printf("[--] no stable reading (valid pings %d/%d) "
                  "-> check wiring / divider / 100uF cap / aim at a surface\n",
                  valid, SAMPLES);
    return;
  }

  // Exponential smoothing on top of the median for a steady display value.
  if (!s_haveEma) { s_ema = median; s_haveEma = true; }
  else            { s_ema = EMA_ALPHA * median + (1.0f - EMA_ALPHA) * s_ema; }

  Serial.printf("Distance: %6.1f cm  (%.2f m)  | median %.1f  smoothed %.1f  "
                "| jitter %.1f cm over %d/%d pings\n",
                s_ema, s_ema / 100.0f, median, s_ema, spread, valid, SAMPLES);
}
