#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_BMI270_Arduino_Library.h>

// Create sensor object
BMI270 bmi;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("Initializing BMI270...");

  // Initialize I2C with your ESP32 pins
  Wire.begin(41, 42);  // SDA = 41, SCL = 42

  // Initialize BMI270
  if (bmi.beginI2C() != BMI2_OK) {
    Serial.println("Could not find a valid BMI270 sensor, check wiring!");
    while (1) delay(10);
  }

  Serial.println("BMI270 initialized!");
}

void loop() {
  // Read sensor data
  bmi.getSensorData();

  // Read acceleration in m/s^2
  float ax = bmi.data.accelX;
  float ay = bmi.data.accelY;
  float az = bmi.data.accelZ;

  // Read gyroscope in rad/s
  float gx = bmi.data.gyroX;
  float gy = bmi.data.gyroY;
  float gz = bmi.data.gyroZ;

  // Read temperature in °C
  float temp = 0.0;
  bmi.getTemperature(&temp);

  // Print results
  Serial.print("Accel (m/s^2) X: "); Serial.print(ax, 2);
  Serial.print(" Y: "); Serial.print(ay, 2);
  Serial.print(" Z: "); Serial.println(az, 2);

  Serial.print("Gyro (rad/s) X: "); Serial.print(gx, 2);
  Serial.print(" Y: "); Serial.print(gy, 2);
  Serial.print(" Z: "); Serial.println(gz, 2);

  Serial.print("Temp (C): "); Serial.println(temp, 2);
  Serial.println("---------------------------");

  delay(500);
}
