/*
  ============================================================
   SWTC \u2014 SCT-013 CURRENT CLAMP TEST  (ESP32 WROOM-32)
  ============================================================

   Sensor: SCT-013 split-core current transformer (CT) clamp, on the SAME
   ADC pin the firmware uses: GPIO35 (ADC1_CH7, input-only).
   Goal: print a STABLE current reading (AC RMS) on the Serial Monitor.

   WHY EARLIER READINGS WERE WRONG (and how this sketch fixes it)
   ----------------------------------------------------------------
   A CT clamp outputs a small AC signal centred on 0V (it swings NEGATIVE),
   but the ESP32 ADC can only read 0..3.3V. Two things are essential:

   1) A DC BIAS ("mid-rail") circuit lifts the AC so it rides on ~1.65V.
      Without it, the negative half is clipped to 0 \u2192 garbage. (Wiring below.)

   2) The reading must be a proper **AC RMS around the true midpoint**. A
      fixed/guessed offset (the firmware hard-codes 1650 mV) is the usual
      cause of "wrong" numbers. This sketch AUTO-ZEROES: it measures the
      real DC mean over the window and computes RMS around it in one pass:

          Vrms = sqrt( mean(x^2) - mean(x)^2 )     [x in mV]

      This removes the DC offset exactly, no matter what it actually is, and
      samples over ~10 mains cycles for a steady result.

   ----------------------------------------------------------------
   WIRING  (bias / mid-rail circuit \u2014 REQUIRED)
   ----------------------------------------------------------------

     3.3V ---[ R1 10k ]---+---[ R2 10k ]--- GND
                          |
                          M (\u2248 1.65V mid-rail)
                          |
              CT lead A --+           C1 10\u00b5F from M to GND (bias decoupling)
              CT lead B ------------- GPIO35 (ADC)

     - SCT-013 leads = tip + sleeve of its 3.5 mm jack.
     - VOLTAGE-output CT (e.g. SCT-013-030 = 30A:1V, has an INTERNAL burden):
       wire exactly as above \u2014 no burden resistor needed.
     - CURRENT-output CT (SCT-013-000 = 100A:50mA, NO internal burden):
       add a BURDEN RESISTOR across the two CT leads (e.g. 33\u201362\u03a9). Pick it
       so the peak stays under ~1.6V: Rb \u2248 (1.6V \u00d7 turns) / (\u221a2 \u00d7 Ipeak).
     - A 10\u00b5F cap from M to GND keeps the mid-rail steady (important!).

   Clamp the CT around ONE conductor only (live OR neutral, not both).

   ----------------------------------------------------------------
   CALIBRATION
   ----------------------------------------------------------------
   The sketch prints AC RMS in millivolts (MCU-side, matches the firmware's
   ctRmsMv) AND an estimated current using CT_AMPS_PER_MV. To calibrate:
     1) Run a KNOWN load (e.g. a 1000W heater \u2248 4.3A @ 230V).
     2) Note the reported "Vrms" (mV).
     3) CT_AMPS_PER_MV = known_amps / Vrms_mV. Set it below and re-flash.
   Rough starting points (depend heavily on your bias/burden):
     - SCT-013-030 (30A:1V): ~0.030 A/mV before divider losses.

   Board: "ESP32 Dev Module"  |  115200 baud
  ============================================================
*/

#include <Arduino.h>
#include <math.h>

// ---- Config ------------------------------------------------
#define CT_PIN            35        // GPIO35 = ADC1_CH7 (same as firmware PIN_CT_ADC)
#define SAMPLE_WINDOW_MS  200       // ~10 mains cycles at 50 Hz (accurate RMS)
#define CT_AMPS_PER_MV    0.030f    // calibration: amps per mV RMS (see header)
#define NOISE_FLOOR_MV    3.0f      // below this, treat as "no load / noise"
#define PRINT_PERIOD_MS   500       // how often to print

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("   SCT-013 Current Clamp Test  (ESP32, GPIO35)"));
  Serial.println(F("=================================================="));
  Serial.printf ("  window=%dms  cal=%.4f A/mV  noise floor=%.1f mV\n",
                 SAMPLE_WINDOW_MS, CT_AMPS_PER_MV, NOISE_FLOOR_MV);
  Serial.println(F("  Needs a mid-rail bias circuit (see header). Auto-zeroes"));
  Serial.println(F("  the DC offset, so no fixed 1650mV assumption."));
  Serial.println(F("--------------------------------------------------"));

  analogReadResolution(12);                       // 0..4095
  analogSetPinAttenuation(CT_PIN, ADC_11db);      // full ~0..3.3V range
}

// Sample for SAMPLE_WINDOW_MS and return AC RMS in mV. Also reports the
// measured DC mid-rail (offset) and the sample count via out-params.
static float measureRmsMv(float* offsetMvOut, uint32_t* nOut) {
  double sum = 0.0, sumSq = 0.0;
  uint32_t n = 0;
  uint32_t t0 = millis();

  while (millis() - t0 < SAMPLE_WINDOW_MS) {
    uint32_t mv = analogReadMilliVolts(CT_PIN);   // calibrated mV
    sum   += (double)mv;
    sumSq += (double)mv * (double)mv;
    n++;
  }

  if (n == 0) { *offsetMvOut = 0; *nOut = 0; return 0.0f; }

  double mean   = sum / n;               // DC mid-rail (auto-zero)
  double meanSq = sumSq / n;
  double var    = meanSq - mean * mean;  // variance = AC power
  if (var < 0) var = 0;

  *offsetMvOut = (float)mean;
  *nOut        = n;
  return (float)sqrt(var);               // AC RMS in mV
}

void loop() {
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint < PRINT_PERIOD_MS) return;
  lastPrint = millis();

  float    offsetMv;
  uint32_t n;
  float    rmsMv = measureRmsMv(&offsetMv, &n);

  // Effective sample rate (diagnostic).
  float sps = (SAMPLE_WINDOW_MS > 0) ? (n * 1000.0f / SAMPLE_WINDOW_MS) : 0;

  if (rmsMv < NOISE_FLOOR_MV) {
    Serial.printf("[--] no load  | Vrms=%.2f mV  offset=%.0f mV  (%.0f sps)  "
                  "-> clamp 1 wire, check bias circuit\n",
                  rmsMv, offsetMv, sps);
    return;
  }

  float amps = rmsMv * CT_AMPS_PER_MV;

  Serial.printf("Current: %6.2f A  | Vrms=%7.2f mV  offset=%4.0f mV  "
                "samples=%lu (%.0f sps)\n",
                amps, rmsMv, offsetMv, (unsigned long)n, sps);
}




/**
 *  Test Values for the CT clamp readings.
 * 

22:16:44.080 -> Current:   0.14 A  | Vrms=   4.57 mV  offset=1390 mV  samples=4637 (23185 sps)
22:16:44.574 -> Current:   0.09 A  | Vrms=   3.16 mV  offset=1412 mV  samples=4637 (23185 sps)
22:16:45.102 -> Current:   0.12 A  | Vrms=   4.10 mV  offset=1412 mV  samples=4637 (23185 sps)
22:16:45.598 -> Current:  23.07 A  | Vrms= 768.91 mV  offset=1409 mV  samples=4635 (23175 sps)
22:16:46.093 -> Current:  21.53 A  | Vrms= 717.76 mV  offset=1412 mV  samples=4637 (23185 sps)
22:16:46.587 -> Current:  17.67 A  | Vrms= 588.85 mV  offset=1414 mV  samples=4637 (23185 sps)
22:16:47.078 -> Current:   8.31 A  | Vrms= 276.89 mV  offset=1412 mV  samples=4637 (23185 sps)
22:16:47.571 -> Current:   8.17 A  | Vrms= 272.32 mV  offset=1412 mV  samples=4637 (23185 sps)
22:16:48.100 -> Current:   8.17 A  | Vrms= 272.26 mV  offset=1411 mV  samples=4638 (23190 sps)

 * 
 */