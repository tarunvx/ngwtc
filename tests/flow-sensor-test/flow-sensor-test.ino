/** Program type 1 - with interrupts
#define SENSOR_PIN 27

// Pulse counter and timing variables
volatile unsigned long pulseCount = 0;
unsigned long oldTime = 0;

// Flow measurement variables
float flowRate = 0.0;          // Flow rate in Liters per minute (L/min)
float totalMilliLitres = 0;    // Cumulative volume in mL
float totalLitres = 0;         // Cumulative volume in L

// Calibration factor for YF-S201: 7.5 pulses/min per L/min
// Formula: Pulse Frequency (Hz) = 7.5 * Flow Rate (L/min)
const float calibrationFactor = 7.5;

// Interrupt Service Routine (ISR) stored in fast IRAM
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

void setup() {
  Serial.begin(115200);

  // Set GPIO 27 as input (external 10k pull-up resistor is attached)
  pinMode(SENSOR_PIN, INPUT_PULLUP);

  // Attach falling-edge interrupt to trigger whenever the sensor pulls signal to GND
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), pulseCounter, FALLING);

  oldTime = millis();
  Serial.println("==========================================");
  Serial.println("YF-S201 Water Flow Sensor Initialized!");
  Serial.println("==========================================");
}

void loop() {
  // Execute measurement update once every 1000ms (1 second)
  if ((millis() - oldTime) >= 1000) {
    
    // Safely read volatile pulse count by briefly detaching the interrupt
    detachInterrupt(digitalPinToInterrupt(SENSOR_PIN));

    // Calculate flow rate in L/min
    // (Pulses in 1 sec) / 7.5 = Flow Rate in L/min
    flowRate = (pulseCount / calibrationFactor);

    // Calculate fluid volume passed during this 1-second interval
    // (L/min / 60 sec) * 1000 mL/L = mL passed in 1 sec
    float flowMilliLitres = (flowRate / 60.0) * 1000.0;

    // Accumulate total volume
    totalMilliLitres += flowMilliLitres;
    totalLitres = totalMilliLitres / 1000.0;

    // Print values to Serial Monitor
    Serial.print("Flow Rate: ");
    Serial.print(flowRate, 2);
    Serial.print(" L/min | ");

    Serial.print("Total Volume: ");
    Serial.print(totalLitres, 3);
    Serial.println(" L");

    // Reset pulse counter and re-enable interrupt for the next interval
    pulseCount = 0;
    oldTime = millis();
    attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), pulseCounter, FALLING);
  }
}

*/

#define SENSOR_PIN 27

// Pin tracking variables
bool lastState = HIGH;
unsigned long lastPulseMicros = 0;
unsigned long currentMicros = 0;

// Flow measurements
float instantaneousFlowRate = 0.0; // In L/min
float totalLitres = 0.0;            // Cumulative volume

// Constant for YF-S201: (1 / 450 L) * 60,000,000 us/min
const float INSTANT_FACTOR = 133333.33; 
const float LITERS_PER_PULSE = 1.0 / 450.0; // ~0.002222 L per pulse

void setup() {
  Serial.begin(115200);

  // Set pin with internal pull-up (plus your external 10k resistor)
  pinMode(SENSOR_PIN, INPUT_PULLUP);

  lastState = digitalRead(SENSOR_PIN);
  lastPulseMicros = micros();

  Serial.println("Instantaneous Non-Interrupt Flow Sensor Active.");
}

void loop() {
  currentMicros = micros();
  bool currentState = digitalRead(SENSOR_PIN);

  // Detect a FALLING edge (transition from HIGH to LOW) via polling
  if (lastState == HIGH && currentState == LOW) {
    
    // Time difference between current pulse and previous pulse (in microseconds)
    unsigned long deltaMicros = currentMicros - lastPulseMicros;

    // Filter out potential contact chatter noise (< 2000 µs / 2ms)
    if (deltaMicros > 2000) {
      
      // Calculate instantaneous flow rate in L/min
      instantaneousFlowRate = INSTANT_FACTOR / (float)deltaMicros;

      // Accumulate total volume
      totalLitres += LITERS_PER_PULSE;

      // Print instantaneous readings immediately on pulse event
      Serial.print("INSTANT Flow: ");
      Serial.print(instantaneousFlowRate, 2);
      Serial.print(" L/min | Pulse Interval: ");
      Serial.print(deltaMicros / 1000.0, 1);
      Serial.print(" ms | Total: ");
      Serial.print(totalLitres, 3);
      Serial.println(" L");

      lastPulseMicros = currentMicros;
    }
  }

  lastState = currentState;

  // Timeout Check: If no pulse is received for over 1.5 seconds (1500000 µs), water has stopped
  if (currentMicros - lastPulseMicros > 1500000 && instantaneousFlowRate > 0.0) {
    instantaneousFlowRate = 0.0;
    Serial.println("Flow Stopped -> Instant Flow: 0.00 L/min");
  }
}