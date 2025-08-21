#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

MAX30105 particleSensor;

// Settings for buffer
#define BUFFER_SIZE 100

uint32_t irBuffer[BUFFER_SIZE]; // Infrared LED sensor data
uint32_t redBuffer[BUFFER_SIZE]; // Red LED sensor data
int32_t bufferLength; // Data length

int32_t spo2; // Calculated SPO2 value
int8_t validSPO2; // Indicator to show if the SPO2 calculation is valid
int32_t heartRate; // Calculated heart rate value
int8_t validHeartRate; // Indicator to show if the heart rate calculation is valid

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Initializing MAX30102...");

  // Initialize sensor with correct I2C pins for Heltec V3
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD, 0x57)) {
    Serial.println("MAX30102 was not found. Please check connections/power.");
    while (1);
  }

  Serial.println("Place your finger on the sensor.");

  particleSensor.setup(); // Configure sensor with default settings
  particleSensor.setPulseAmplitudeRed(0x0A); // Turn Red LED low to avoid blinding
  particleSensor.setPulseAmplitudeIR(0x0A);  // Turn IR LED low
}

void loop() {
  bufferLength = BUFFER_SIZE; // Collect 100 samples

  // Read first set of samples
  for (int i = 0; i < bufferLength; i++) {
    while (!particleSensor.available()) {
      particleSensor.check();
    }
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }

  // Run the algorithm
  maxim_heart_rate_and_oxygen_saturation(
      irBuffer, bufferLength,
      redBuffer,
      &spo2, &validSPO2,
      &heartRate, &validHeartRate);

  Serial.print("Heart Rate: ");
  if (validHeartRate)
    Serial.print(heartRate);
  else
    Serial.print("Invalid");

  Serial.print(" bpm | SpO2: ");
  if (validSPO2)
    Serial.print(spo2);
  else
    Serial.print("Invalid");

  Serial.println(" %");

  delay(1000); // Small pause before next reading
}
