#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"
#include "Adafruit_AS726x.h"

MAX30105 particleSensor;
Adafruit_AS726x as7263;

// MAX30102 settings
#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int32_t bufferLength;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// --- Placeholder regression functions ---
// Replace coefficients with trained values!
float estimateGlucose(float ch1, float ch2, float ch3) {
  return 80.0 + 0.05*ch1 - 0.03*ch2 + 0.02*ch3;
}

float estimateSystolicBP(float ch1, float ch4, float ch6) {
  return 110.0 + 0.04*ch1 + 0.01*ch4 - 0.02*ch6;
}

float estimateDiastolicBP(float ch2, float ch5) {
  return 70.0 + 0.03*ch2 - 0.015*ch5;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Initializing MAX30102...");
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD, 0x57)) {
    Serial.println("MAX30102 not found. Check wiring.");
    while (1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeIR(0x0A);

  Serial.println("Initializing AS7263...");
  if (!as7263.begin()) {
    Serial.println("AS7263 not found. Check wiring.");
    while (1);
  }
  as7263.setIntegrationTime(100);
  as7263.setGain(3);

  Serial.println("Place your finger on the sensors...");
}

void loop() {
  // --- MAX30102 HR + SpO2 ---
  bufferLength = BUFFER_SIZE;
  for (int i = 0; i < bufferLength; i++) {
    while (!particleSensor.available()) {
      particleSensor.check();
    }
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }
  maxim_heart_rate_and_oxygen_saturation(
      irBuffer, bufferLength,
      redBuffer,
      &spo2, &validSPO2,
      &heartRate, &validHeartRate);

  Serial.print("Heart Rate: ");
  if (validHeartRate) Serial.print(heartRate);
  else Serial.print("Invalid");
  Serial.print(" bpm | SpO2: ");
  if (validSPO2) Serial.print(spo2);
  else Serial.print("Invalid");
  Serial.println(" %");

  // --- AS7263 Blood Glucose & BP ---
  as7263.startMeasurement();
  delay(750); // wait for measurement
  uint16_t channels[6];
  for (int i = 0; i < 6; i++) {
    channels[i] = as7263.readChannel(i);
  }

  float glucose = estimateGlucose(channels[0], channels[1], channels[2]);
  float sysBP   = estimateSystolicBP(channels[0], channels[3], channels[5]);
  float diaBP   = estimateDiastolicBP(channels[1], channels[4]);

  Serial.print("Estimated Glucose: ");
  Serial.print(glucose);
  Serial.print(" mg/dL | SysBP: ");
  Serial.print(sysBP);
  Serial.print(" mmHg | DiaBP: ");
  Serial.print(diaBP);
  Serial.println(" mmHg");

  delay(2000);
}
