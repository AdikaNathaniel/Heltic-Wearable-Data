#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h" // SparkFun MAX3010x library

MAX30105 particleSensor;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Initializing MAX30102 on SDA=41, SCL=42...");

  // Start I2C on your custom pins
  Wire.begin(41, 42);

  // Initialize sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX30102 was not found. Please check wiring/power.");
    while (1); // Halt if not found
  }

  // Configure the sensor with default settings
  particleSensor.setup(); // Use default settings for HR & SpO2
  particleSensor.setPulseAmplitudeRed(0xFF);    // Red LED ON (max brightness)
  particleSensor.setPulseAmplitudeIR(0xFF);     // IR LED ON (max brightness)
  particleSensor.setPulseAmplitudeGreen(0x00);  // Green LED OFF

  Serial.println("MAX30102 initialized successfully!");
  Serial.println("Red + IR LEDs should now be ON at maximum brightness.");
}

void loop() {
  long irValue = particleSensor.getIR();   // IR LED value
  long redValue = particleSensor.getRed(); // Red LED value

  Serial.print("IR: ");
  Serial.print(irValue);
  Serial.print("\tRed: ");
  Serial.println(redValue);

  delay(500); // Read every 0.5 seconds
}
