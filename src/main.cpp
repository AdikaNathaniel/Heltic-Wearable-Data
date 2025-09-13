#include <Arduino.h>
#include <Wire.h>

#define TMP117_ADDR 0x48   // Default I2C address
#define TMP117_TEMP_REG 0x00
#define CALIBRATION_OFFSET 3.0   // Adjust this after testing (2.0–4.0 °C works for most cases)

// Function to read raw temperature from TMP117
float readTMP117() {
  Wire.beginTransmission(TMP117_ADDR);
  Wire.write(TMP117_TEMP_REG);
  Wire.endTransmission(false);

  Wire.requestFrom(TMP117_ADDR, (uint8_t)2);

  if (Wire.available() == 2) {
    uint16_t raw = (Wire.read() << 8) | Wire.read();

    // TMP117: signed 16-bit value, 1 LSB = 1/128 °C
    float temperature = (int16_t)raw / 128.0;
    return temperature;
  }

  return NAN; // Not a number if read failed
}

// Function to get an averaged reading
float getAverageTemp(int samples = 10) {
  float sum = 0;
  int valid = 0;
  for (int i = 0; i < samples; i++) {
    float t = readTMP117();
    if (!isnan(t)) {
      sum += t;
      valid++;
    }
    delay(100); // small delay between samples
  }
  return (valid > 0) ? sum / valid : NAN;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(41, 42);  // SDA=41, SCL=42 for your Heltec ESP32

  Serial.println("TMP117 Body Temperature Measurement");
}

void loop() {
  float skinTemp = getAverageTemp();
  if (!isnan(skinTemp)) {
    // Apply calibration offset to estimate body temperature
    float bodyTemp = skinTemp + CALIBRATION_OFFSET;

    Serial.print("Skin Temperature: ");
    Serial.print(skinTemp, 2);
    Serial.print(" °C | Estimated Body Temperature: ");
    Serial.print(bodyTemp, 2);
    Serial.println(" °C");
  } else {
    Serial.println("Failed to read TMP117");
  }

  delay(2000); // 2 seconds between readings
}
