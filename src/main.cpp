#include <Arduino.h>
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial);  // wait for serial (useful on some boards)
  Serial.println("\nI2C Scanner");

  // Explicitly set your custom SDA/SCL pins here:
  Wire.begin(41, 42);   // SDA = 41, SCL = 42

  delay(100);
}

void loop() {
  Serial.println("Scanning I2C addresses...");
  int nDevices = 0;

  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address, HEX);
      Serial.println("  !");
      nDevices++;
    } else if (error == 4) {
      Serial.print("Unknown error at 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }

  if (nDevices == 0) {
    Serial.println("No I2C devices found\n");
  } else {
    Serial.print("Done. ");
    Serial.print(nDevices);
    Serial.println(" device(s) found\n");
  }

  delay(5000); // wait 5s between scans
}
