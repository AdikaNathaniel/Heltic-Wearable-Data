#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"
#include "Adafruit_AS726x.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SparkFun_BMI270_Arduino_Library.h>

// Sensor objects
MAX30105 particleSensor;
Adafruit_AS726x as7263;
BMI270 bmi;

// WiFi credentials
const char* ssid = "Network";
const char* password = "jehovahofmercy#love";

// API endpoint
const char* serverURL = "http://localhost:3100/api/v1/vitals-health-data";

// MAX30102 settings
#define HR_BUFFER_SIZE 100
uint32_t irBuffer[HR_BUFFER_SIZE];
uint32_t redBuffer[HR_BUFFER_SIZE];
int32_t bufferLength;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Operation states
enum OperationState {
  AS7263_PHASE,
  MAX30102_PHASE,
  POST_DATA_PHASE
};
OperationState currentState = AS7263_PHASE;

// Variables for 2-minute average calculation
const unsigned long PHASE_DURATION = 120000; // 2 minutes in milliseconds
unsigned long phaseStartTime;

// Arrays to store readings for averaging
const int MAX_READINGS = 120; // About 120 readings in 2 minutes
float glucoseReadings[MAX_READINGS];
float sysBPReadings[MAX_READINGS];
float diaBPReadings[MAX_READINGS];
int hrReadings[MAX_READINGS];
int spo2Readings[MAX_READINGS];
float accelXReadings[MAX_READINGS];
float accelYReadings[MAX_READINGS];
float accelZReadings[MAX_READINGS];
float gyroXReadings[MAX_READINGS];
float gyroYReadings[MAX_READINGS];
float gyroZReadings[MAX_READINGS];
int readingCount = 0;

// Variables to store final averages
float avgGlucose = 0;
float avgSysBP = 0;
float avgDiaBP = 0;
float avgHR = 0;
float avgSPO2 = 0;
float avgAccelX = 0;
float avgAccelY = 0;
float avgAccelZ = 0;
float avgGyroX = 0;
float avgGyroY = 0;
float avgGyroZ = 0;

// Function declarations
void postVitalsDataToServer(float glucose, float sysBP, float diaBP, float heartRate, float spO2, 
                           float accelX, float accelY, float accelZ, 
                           float gyroX, float gyroY, float gyroZ);
void resetReadings();

// --- Placeholder regression functions ---
// Replace coefficients with trained values!
float estimateGlucose(float ch1, float ch2, float ch3) {
  return 80.0 + 0.05 * ch1 - 0.03 * ch2 + 0.02 * ch3;
}

float estimateSystolicBP(float ch1, float ch4, float ch6) {
  return 110.0 + 0.04 * ch1 + 0.01 * ch4 - 0.02 * ch6;
}

float estimateDiastolicBP(float ch2, float ch5) {
  return 70.0 + 0.03 * ch2 - 0.015 * ch5;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Initialize I2C with ESP32 pins
  Wire.begin(41, 42);  // SDA = 41, SCL = 42
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  
  // Initialize MAX30102
  Serial.println("Initializing MAX30102...");
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD, 0x57)) {
    Serial.println("MAX30102 not found. Check wiring.");
    while (1);
  }
  
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeIR(0x0A);
  
  // Initialize AS7263
  Serial.println("Initializing AS7263...");
  if (!as7263.begin()) {
    Serial.println("AS7263 not found. Check wiring.");
    while (1);
  }
  
  as7263.setIntegrationTime(100);
  as7263.setGain(3);
  
  // Initialize BMI270
  Serial.println("Initializing BMI270...");
  if (bmi.beginI2C() != BMI2_OK) {
    Serial.println("Could not find a valid BMI270 sensor, check wiring!");
    while (1) delay(10);
  }
  Serial.println("BMI270 initialized!");
  
  Serial.println("Starting with AS7263 sensor for 2 minutes...");
  Serial.println("Place your finger on the AS7263 sensor.");
  phaseStartTime = millis();
  currentState = AS7263_PHASE;
}

void loop() {
  unsigned long currentTime = millis();
  
  switch (currentState) {
    case AS7263_PHASE:
      // AS7263 phase: 2 minutes of data collection
      if (currentTime - phaseStartTime < PHASE_DURATION) {
        // --- AS7263 Blood Glucose & BP ---
        as7263.startMeasurement();
        delay(750); // wait for measurement
        uint16_t channels[6];
        for (int i = 0; i < 6; i++) {
          channels[i] = as7263.readChannel(i);
        }
        
        float glucose = estimateGlucose(channels[0], channels[1], channels[2]);
        float sysBP = estimateSystolicBP(channels[0], channels[3], channels[5]);
        float diaBP = estimateDiastolicBP(channels[1], channels[4]);
        
        // --- BMI270 Motion Data ---
        bmi.getSensorData();
        float ax = bmi.data.accelX;
        float ay = bmi.data.accelY;
        float az = bmi.data.accelZ;
        float gx = bmi.data.gyroX;
        float gy = bmi.data.gyroY;
        float gz = bmi.data.gyroZ;
        
        // Store readings for averaging
        if (readingCount < MAX_READINGS) {
          glucoseReadings[readingCount] = glucose;
          sysBPReadings[readingCount] = sysBP;
          diaBPReadings[readingCount] = diaBP;
          accelXReadings[readingCount] = ax;
          accelYReadings[readingCount] = ay;
          accelZReadings[readingCount] = az;
          gyroXReadings[readingCount] = gx;
          gyroYReadings[readingCount] = gy;
          gyroZReadings[readingCount] = gz;
          readingCount++;
        }
        
        // Display current readings
        Serial.print("AS7263 - Time remaining: ");
        Serial.print((PHASE_DURATION - (currentTime - phaseStartTime)) / 1000);
        Serial.println(" seconds");
        
        Serial.print("Glucose: ");
        Serial.print(glucose);
        Serial.print(" mg/dL | SysBP: ");
        Serial.print(sysBP);
        Serial.print(" mmHg | DiaBP: ");
        Serial.print(diaBP);
        Serial.println(" mmHg");
        
        Serial.print("Accel (m/s^2) X: "); Serial.print(ax, 2);
        Serial.print(" Y: "); Serial.print(ay, 2);
        Serial.print(" Z: "); Serial.println(az, 2);
        
        Serial.print("Gyro (rad/s) X: "); Serial.print(gx, 2);
        Serial.print(" Y: "); Serial.print(gy, 2);
        Serial.print(" Z: "); Serial.println(gz, 2);
        Serial.println("---------------------------");
        
        delay(2000); // Wait 2 seconds between readings
      } else {
        // 2 minutes have passed - calculate and display averages
        avgGlucose = 0;
        avgSysBP = 0;
        avgDiaBP = 0;
        avgAccelX = 0;
        avgAccelY = 0;
        avgAccelZ = 0;
        avgGyroX = 0;
        avgGyroY = 0;
        avgGyroZ = 0;
        
        for (int i = 0; i < readingCount; i++) {
          avgGlucose += glucoseReadings[i];
          avgSysBP += sysBPReadings[i];
          avgDiaBP += diaBPReadings[i];
          avgAccelX += accelXReadings[i];
          avgAccelY += accelYReadings[i];
          avgAccelZ += accelZReadings[i];
          avgGyroX += gyroXReadings[i];
          avgGyroY += gyroYReadings[i];
          avgGyroZ += gyroZReadings[i];
        }
        
        avgGlucose /= readingCount;
        avgSysBP /= readingCount;
        avgDiaBP /= readingCount;
        avgAccelX /= readingCount;
        avgAccelY /= readingCount;
        avgAccelZ /= readingCount;
        avgGyroX /= readingCount;
        avgGyroY /= readingCount;
        avgGyroZ /= readingCount;
        
        Serial.println("\n=== AS7263 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Glucose: ");
        Serial.print(avgGlucose);
        Serial.print(" mg/dL | Average SysBP: ");
        Serial.print(avgSysBP);
        Serial.print(" mmHg | Average DiaBP: ");
        Serial.print(avgDiaBP);
        Serial.println(" mmHg");
        
        Serial.print("Average Accel (m/s^2) X: "); Serial.print(avgAccelX, 2);
        Serial.print(" Y: "); Serial.print(avgAccelY, 2);
        Serial.print(" Z: "); Serial.println(avgAccelZ, 2);
        
        Serial.print("Average Gyro (rad/s) X: "); Serial.print(avgGyroX, 2);
        Serial.print(" Y: "); Serial.print(avgGyroY, 2);
        Serial.print(" Z: "); Serial.println(avgGyroZ, 2);
        Serial.println("========================================");
        
        // Reset for next phase
        resetReadings();
        Serial.println("Now switching to MAX30102 sensor for 2 minutes...");
        Serial.println("Place your finger on the MAX30102 sensor.");
        phaseStartTime = millis();
        currentState = MAX30102_PHASE;
      }
      break;
      
    case MAX30102_PHASE:
      // MAX30102 phase: 2 minutes of data collection
      if (currentTime - phaseStartTime < PHASE_DURATION) {
        // --- MAX30102 HR + SpO2 ---
        bufferLength = HR_BUFFER_SIZE;
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
        
        // --- BMI270 Motion Data ---
        bmi.getSensorData();
        float ax = bmi.data.accelX;
        float ay = bmi.data.accelY;
        float az = bmi.data.accelZ;
        float gx = bmi.data.gyroX;
        float gy = bmi.data.gyroY;
        float gz = bmi.data.gyroZ;
        
        // Store readings for averaging
        if (readingCount < MAX_READINGS) {
          if (validHeartRate) hrReadings[readingCount] = heartRate;
          if (validSPO2) spo2Readings[readingCount] = spo2;
          accelXReadings[readingCount] = ax;
          accelYReadings[readingCount] = ay;
          accelZReadings[readingCount] = az;
          gyroXReadings[readingCount] = gx;
          gyroYReadings[readingCount] = gy;
          gyroZReadings[readingCount] = gz;
          readingCount++;
        }
        
        // Display current readings
        Serial.print("MAX30102 - Time remaining: ");
        Serial.print((PHASE_DURATION - (currentTime - phaseStartTime)) / 1000);
        Serial.println(" seconds");
        
        Serial.print("Heart Rate: ");
        if (validHeartRate) Serial.print(heartRate);
        else Serial.print("Invalid");
        Serial.print(" bpm | SpO2: ");
        if (validSPO2) Serial.print(spo2);
        else Serial.print("Invalid");
        Serial.println(" %");
        
        Serial.print("Accel (m/s^2) X: "); Serial.print(ax, 2);
        Serial.print(" Y: "); Serial.print(ay, 2);
        Serial.print(" Z: "); Serial.println(az, 2);
        
        Serial.print("Gyro (rad/s) X: "); Serial.print(gx, 2);
        Serial.print(" Y: "); Serial.print(gy, 2);
        Serial.print(" Z: "); Serial.println(gz, 2);
        Serial.println("---------------------------");
        
        delay(1000); // Wait 1 second between readings
      } else {
        // 2 minutes have passed - calculate and display averages
        avgHR = 0;
        avgSPO2 = 0;
        avgAccelX = 0;
        avgAccelY = 0;
        avgAccelZ = 0;
        avgGyroX = 0;
        avgGyroY = 0;
        avgGyroZ = 0;
        
        int validHRCount = 0, validSPO2Count = 0;
        
        for (int i = 0; i < readingCount; i++) {
          if (hrReadings[i] > 0) {
            avgHR += hrReadings[i];
            validHRCount++;
          }
          
          if (spo2Readings[i] > 0) {
            avgSPO2 += spo2Readings[i];
            validSPO2Count++;
          }
          
          avgAccelX += accelXReadings[i];
          avgAccelY += accelYReadings[i];
          avgAccelZ += accelZReadings[i];
          avgGyroX += gyroXReadings[i];
          avgGyroY += gyroYReadings[i];
          avgGyroZ += gyroZReadings[i];
        }
        
        if (validHRCount > 0) avgHR /= validHRCount;
        if (validSPO2Count > 0) avgSPO2 /= validSPO2Count;
        
        avgAccelX /= readingCount;
        avgAccelY /= readingCount;
        avgAccelZ /= readingCount;
        avgGyroX /= readingCount;
        avgGyroY /= readingCount;
        avgGyroZ /= readingCount;
        
        Serial.println("\n=== MAX30102 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Heart Rate: ");
        Serial.print(avgHR);
        Serial.print(" bpm | Average SpO2: ");
        Serial.print(avgSPO2);
        Serial.println(" %");
        
        Serial.print("Average Accel (m/s^2) X: "); Serial.print(avgAccelX, 2);
        Serial.print(" Y: "); Serial.print(avgAccelY, 2);
        Serial.print(" Z: "); Serial.println(avgAccelZ, 2);
        
        Serial.print("Average Gyro (rad/s) X: "); Serial.print(avgGyroX, 2);
        Serial.print(" Y: "); Serial.print(avgGyroY, 2);
        Serial.print(" Z: "); Serial.println(avgGyroZ, 2);
        Serial.println("==========================================");
        
        // Move to post data phase
        currentState = POST_DATA_PHASE;
      }
      break;
      
    case POST_DATA_PHASE:
      // Post all data to server
      Serial.println("\n=== POSTING ALL VITAL SIGNS TO SERVER ===");
      postVitalsDataToServer(avgGlucose, avgSysBP, avgDiaBP, avgHR, avgSPO2,
                            avgAccelX, avgAccelY, avgAccelZ,
                            avgGyroX, avgGyroY, avgGyroZ);
      
      // Reset for next cycle
      resetReadings();
      Serial.println("Starting new cycle with AS7263 sensor for 2 minutes...");
      Serial.println("Place your finger on the AS7263 sensor.");
      phaseStartTime = millis();
      currentState = AS7263_PHASE;
      break;
  }
}

void resetReadings() {
  // Reset all reading arrays and counters
  readingCount = 0;
  for (int i = 0; i < MAX_READINGS; i++) {
    glucoseReadings[i] = 0;
    sysBPReadings[i] = 0;
    diaBPReadings[i] = 0;
    hrReadings[i] = 0;
    spo2Readings[i] = 0;
    accelXReadings[i] = 0;
    accelYReadings[i] = 0;
    accelZReadings[i] = 0;
    gyroXReadings[i] = 0;
    gyroYReadings[i] = 0;
    gyroZReadings[i] = 0;
  }
}

void postVitalsDataToServer(float glucose, float sysBP, float diaBP, float heartRate, float spO2,
                           float accelX, float accelY, float accelZ,
                           float gyroX, float gyroY, float gyroZ) {
  // Create JSON object
  JsonDocument doc;
  
  doc["userId"] = "user-001-wearable";
  doc["inputMethod"] = "wearable";
  
  JsonObject bloodPressure = doc["bloodPressure"].to<JsonObject>();
  bloodPressure["systolic"] = sysBP;
  bloodPressure["diastolic"] = diaBP;
  
  doc["heartRate"] = heartRate;
  doc["spO2"] = spO2;
  doc["bloodGlucose"] = glucose;
  
  // Add motion data
  JsonObject motion = doc["motion"].to<JsonObject>();
  JsonObject acceleration = motion["acceleration"].to<JsonObject>();
  acceleration["x"] = accelX;
  acceleration["y"] = accelY;
  acceleration["z"] = accelZ;
  
  JsonObject gyroscope = motion["gyroscope"].to<JsonObject>();
  gyroscope["x"] = gyroX;
  gyroscope["y"] = gyroY;
  gyroscope["z"] = gyroZ;
  
  // Serialize JSON to string
  String jsonString;
  serializeJson(doc, jsonString);
  
  // Send HTTP POST request
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");
    
    Serial.print("Posting ALL data to server: ");
    Serial.println(jsonString);
    
    int httpResponseCode = http.POST(jsonString);
    
    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
      Serial.print("Server response: ");
      Serial.println(response);
      Serial.println("All vital signs posted successfully!");
    } else {
      Serial.print("Error posting data. Error code: ");
      Serial.println(httpResponseCode);
    }
    
    http.end();
  } else {
    Serial.println("WiFi disconnected. Cannot post data.");
    // Try to reconnect
    WiFi.begin(ssid, password);
    Serial.print("Attempting to reconnect to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnected to WiFi");
      // Try posting again
      postVitalsDataToServer(glucose, sysBP, diaBP, heartRate, spO2,
                            accelX, accelY, accelZ,
                            gyroX, gyroY, gyroZ);
    } else {
      Serial.println("\nFailed to reconnect to WiFi");
    }
  }
}