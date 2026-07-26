#define PRESSURE_PIN 34

// Resistor divider values (in Ohms)
const float R1 = 15000.0; // 10k resistor connected to Sensor Signal
const float R2 = 30000.0; // 20k resistor connected to GND

// Divider scaling factor: V_sensor = V_adc * ((R1 + R2) / R2)
const float DIVIDER_RATIO = (R1 + R2) / R2; // = 1.5

// Sensor Specifications
const float V_MIN = 0.5;   // Output voltage at 0 MPa (5V powered)
const float V_MAX = 4.5;   // Output voltage at 1.0 MPa
const float P_MAX = 1.0;   // Max pressure in MPa (1MPa = 10 Bar)

void setup() {
  Serial.begin(115200);
  
  // Configure ADC pin (GPIO 34) with 11dB attenuation for full 0-3.3V range
  analogSetAttenuation(ADC_11db);
  pinMode(PRESSURE_PIN, INPUT);

  Serial.println("==========================================");
  Serial.println("G1/4 1MPa Stainless Steel Pressure Sensor Active");
  Serial.println("==========================================");
}

void loop() {
  // Take 20 ADC readings and average them to eliminate electronic noise
  unsigned long totalMilliVolts = 0;
  const int samples = 20;

  for (int i = 0; i < samples; i++) {
    totalMilliVolts += analogReadMilliVolts(PRESSURE_PIN);
    delay(5);
  }

  // Calculate averaged ADC voltage at Pin 34 in Volts
  float pinVoltage = (totalMilliVolts / (float)samples) / 1000.0;

  // Calculate actual raw sensor output voltage before the divider
  float sensorVoltage = pinVoltage * DIVIDER_RATIO;

  // Calculate pressure in MPa based on linear formula: (V_out - 0.5) / 4.0 * P_max
  float pressureMPa = ((sensorVoltage - V_MIN) / (V_MAX - V_MIN)) * P_MAX;

  // Clamp small negative values (due to minor ADC drift at 0 pressure) to 0
  if (pressureMPa < 0.0) {
    pressureMPa = 0.0;
  }

  // Convert to common units
  float pressureBar = pressureMPa * 10.0;     // 1 MPa = 10 Bar
  float pressurePSI = pressureMPa * 145.038;  // 1 MPa = 145.038 PSI

  // Serial Monitor Output
  Serial.print("ADC Pin: ");
  Serial.print(pinVoltage, 2);
  Serial.print(" V | Sensor: ");
  Serial.print(sensorVoltage, 2);
  Serial.print(" V | Pressure: ");
  Serial.print(pressureBar, 2);
  Serial.print(" Bar (");
  Serial.print(pressurePSI, 1);
  Serial.println(" PSI)");

  delay(500); // Update twice per second
}