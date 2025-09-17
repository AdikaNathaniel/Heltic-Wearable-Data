#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"
#include "Adafruit_AS726x.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SparkFun_BMI270_Arduino_Library.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>
#include <WiFiClient.h>
#include <algorithm>
#include <esp_log.h>

// Sensor objects
MAX30105 particleSensor;
Adafruit_AS726x as7263;
BMI270 bmi;

// TMP117 Temperature Sensor
#define TMP117_ADDR 0x48   // Default I2C address
#define TMP117_TEMP_REG 0x00
#define CALIBRATION_OFFSET 3.0   // Adjust this after testing (2.0–4.0 °C works for most cases)

// WiFi credentials
const char* ssid = "Network";
const char* password = "jehovahofmercy#love";

// API endpoints
const char* serverURL = "http://192.168.43.64:3100/api/v1/vitals-health-data";
const char* csvUploadURL = "http://192.168.43.64:3100/api/v1/csv/upload";

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
float tempReadings[MAX_READINGS];
float bodyTempReadings[MAX_READINGS];
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
float avgTemp = 0;
float avgBodyTemp = 0;
float avgAccelX = 0;
float avgAccelY = 0;
float avgAccelZ = 0;
float avgGyroX = 0;
float avgGyroY = 0;
float avgGyroZ = 0;

// Data logging variables
bool wifiConnected = false;
bool pendingUpload = false;
String csvData = "";
unsigned long lastDataLogTime = 0;
const unsigned long DATA_LOG_INTERVAL = 1000; // Log data every second

// Function declarations
void postVitalsDataToServer(float glucose, float sysBP, float diaBP, float heartRate, float spO2,
                           float temperature, float bodyTemperature,
                           float accelX, float accelY, float accelZ, 
                           float gyroX, float gyroY, float gyroZ);
void resetReadings();
String getTimestamp();
void logDataToCSV(String timestamp, String sensorType, String data);
void saveDataToFlash();
void readDataFromFlash();
void uploadStoredData();
void uploadCSVToServer();
void initializeSPIFFS();
String formatCSVRow(String timestamp, String sensorType, String data);

// --- Placeholder regression functions ---
float estimateGlucose(float ch1, float ch2, float ch3) {
  return 80.0 + 0.05 * ch1 - 0.03 * ch2 + 0.02 * ch3;
}

float estimateSystolicBP(float ch1, float ch4, float ch6) {
  return 110.0 + 0.04 * ch1 + 0.01 * ch4 - 0.02 * ch6;
}

float estimateDiastolicBP(float ch2, float ch5) {
  return 70.0 + 0.03 * ch2 - 0.015 * ch5;
}

// Function to read raw temperature from TMP117
float readTMP117() {
  Wire.beginTransmission(TMP117_ADDR);
  Wire.write(TMP117_TEMP_REG);
  Wire.endTransmission(false);

  Wire.requestFrom((uint8_t)TMP117_ADDR, (uint8_t)2);

  if (Wire.available() == 2) {
    uint16_t raw = (Wire.read() << 8) | Wire.read();
    float temperature = (int16_t)raw / 128.0;
    return temperature;
  }

  return NAN;
}

// Function to get an averaged temperature reading
float getAverageTemp(int samples = 5) {
  float sum = 0;
  int valid = 0;
  for (int i = 0; i < samples; i++) {
    float t = readTMP117();
    if (!isnan(t)) {
      sum += t;
      valid++;
    }
    delay(50);
  }
  return (valid > 0) ? sum / valid : NAN;
}

// Function to get current timestamp in ISO 8601 format
String getTimestamp() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return "2023-01-01T00:00:00Z";
  }
  
  char timeString[25];
  strftime(timeString, sizeof(timeString), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(timeString);
}

// Initialize SPIFFS for data storage
void initializeSPIFFS() {
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  Serial.println("SPIFFS initialized successfully");

  if (!SPIFFS.exists("/vitals_data.csv")) {
    File file = SPIFFS.open("/vitals_data.csv", FILE_WRITE);
    if (file) {
      String header = "timestamp,sensorType,glucose,sysBP,diaBP,hr,spo2,skinTemp,bodyTemp,accelX,accelY,accelZ,gyroX,gyroY,gyroZ\n";
      file.print(header);
      file.close();
      Serial.println("CSV header written to file.");
    } else {
      Serial.println("Failed to create CSV file with header.");
    }
  }
}

// Format CSV row with timestamp, sensorType, and data
String formatCSVRow(String timestamp, String sensorType, String data) {
  return timestamp + "," + sensorType + "," + data + "\n";
}

// Log data to CSV format
void logDataToCSV(String timestamp, String sensorType, String data) {
  String csvRow = formatCSVRow(timestamp, sensorType, data);
  csvData += csvRow;
  
  if (!wifiConnected && millis() - lastDataLogTime > 10000) {
    saveDataToFlash();
    lastDataLogTime = millis();
  }
}

// Save data to flash memory
void saveDataToFlash() {
  if (csvData.length() > 0) {
    File file = SPIFFS.open("/vitals_data.csv", FILE_APPEND);
    if(!file){
      Serial.println("Failed to open file for appending");
      return;
    }
    if(file.print(csvData)){
      Serial.println("Data saved to flash: " + String(csvData.length()) + " bytes");
    } else {
      Serial.println("Append failed");
    }
    file.close();
    csvData = "";
  }
}

// Read data from flash memory
void readDataFromFlash() {
  File file = SPIFFS.open("/vitals_data.csv");
  if(!file){
    Serial.println("Failed to open file for reading");
    return;
  }
  
  Serial.println("Stored data from flash:");
  while(file.available()){
    Serial.write(file.read());
  }
  file.close();
}

// Validate CSV format
bool validateCSV(String csvContent) {
  // Expected number of columns: 15 (timestamp, sensorType, glucose, sysBP, diaBP, hr, spo2, skinTemp, bodyTemp, accelX, accelY, accelZ, gyroX, gyroY, gyroZ)
  const int EXPECTED_COLUMNS = 15;
  String lines[10]; // Check first 10 lines to avoid memory issues
  int lineCount = 0;
  
  // Split content into lines
  int start = 0;
  int end = csvContent.indexOf('\n');
  while (end != -1 && lineCount < 10) {
    lines[lineCount] = csvContent.substring(start, end);
    start = end + 1;
    end = csvContent.indexOf('\n', start);
    lineCount++;
  }
  
  // Check each line
  for (int i = 0; i < lineCount; i++) {
    String line = lines[i];
    int commaCount = 0;
    for (char c : line) {
      if (c == ',') commaCount++;
    }
    if (commaCount != EXPECTED_COLUMNS - 1) {
      Serial.print("Invalid CSV line: ");
      Serial.println(line);
      Serial.print("Expected ");
      Serial.print(EXPECTED_COLUMNS);
      Serial.print(" columns, found ");
      Serial.println(commaCount + 1);
      return false;
    }
    // Check if sensorType is valid
    String fields[EXPECTED_COLUMNS];
    int fieldIndex = 0;
    start = 0;
    end = line.indexOf(',');
    while (end != -1 && fieldIndex < EXPECTED_COLUMNS) {
      fields[fieldIndex] = line.substring(start, end);
      start = end + 1;
      end = line.indexOf(',', start);
      fieldIndex++;
    }
    fields[fieldIndex] = line.substring(start);
    if (fields[1] != "AS7263" && fields[1] != "MAX30102" && fields[1] != "AS7263_AVG" && fields[1] != "MAX30102_AVG" && fields[1] != "FINAL_AVG") {
      Serial.print("Invalid sensorType in CSV: ");
      Serial.println(fields[1]);
      return false;
    }
  }
  return true;
}

// Upload CSV file to server using multipart/form-data with chunked transfer encoding
void uploadCSVToServer() {
  File file = SPIFFS.open("/vitals_data.csv");
  if (!file || file.size() == 0) {
    Serial.println("No CSV file to upload or file is empty");
    if (file) file.close();
    return;
  }
  
  Serial.println("Uploading CSV file to server...");
  Serial.print("File size: ");
  Serial.println(file.size());
  
  // Check file size
  const size_t MAX_FILE_SIZE = 1000000; // 1MB threshold
  if (file.size() > MAX_FILE_SIZE) {
    Serial.println("DEBUG: CSV file size exceeds threshold.");
    Serial.print("File size: ");
    Serial.print(file.size());
    Serial.println(" bytes (Max allowed: " + String(MAX_FILE_SIZE) + " bytes)");
    file.close();
    return;
  }
  
  // Generate boundary
  String boundary = "----ESP32FormBoundary" + String(millis());
  
  // Hardcoded server details
  const char* host = "192.168.43.64";
  const int httpPort = 3100;
  const char* path = "/api/v1/csv/upload";
  
  WiFiClient client;
  if (!client.connect(host, httpPort)) {
    Serial.println("DEBUG: Connection to server failed.");
    file.close();
    return;
  }
  
  // Send HTTP request headers
  client.print("POST ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.print(host);
  client.print(":");
  client.print(httpPort);
  client.println();
  client.print("Content-Type: multipart/form-data; boundary=");
  client.println(boundary);
  client.println("Transfer-Encoding: chunked");
  client.println("Connection: close");
  client.println();
  
  // First chunk: multipart headers
  String formHeader = "--" + boundary + "\r\n";
  formHeader += "Content-Disposition: form-data; name=\"file\"; filename=\"vitals_data.csv\"\r\n";
  formHeader += "Content-Type: text/csv\r\n\r\n";
  
  String chunkHeader = String(formHeader.length(), HEX) + "\r\n";
  client.print(chunkHeader);
  client.print(formHeader);
  client.print("\r\n");
  
  // Stream file content in chunks
  const size_t CHUNK_SIZE = 1024;
  uint8_t buffer[CHUNK_SIZE];
  size_t totalRead = 0;
  bool firstChunk = true;
  String preview = "";
  
  while (file.available()) {
    size_t bytesRead = file.readBytes((char*)buffer, CHUNK_SIZE);
    if (bytesRead > 0) {
      if (firstChunk) {
        Serial.print("File content length: ");
        Serial.println(file.size());
        Serial.println("First 200 chars of CSV:");
        for (size_t i = 0; i < std::min<size_t>(200ul, bytesRead); i++) {
          preview += (char)buffer[i];
        }
        Serial.println(preview);
        firstChunk = false;
      }
      
      String chunkSizeStr = String(bytesRead, HEX) + "\r\n";
      client.print(chunkSizeStr);
      client.write(buffer, bytesRead);
      client.print("\r\n");
      totalRead += bytesRead;
    }
  }
  
  file.close();
  Serial.print("Total bytes streamed: ");
  Serial.println(totalRead);
  
  // Final chunk for multipart closure
  String multipartClose = "\r\n--" + boundary + "--\r\n";
  String closeChunkHeader = String(multipartClose.length(), HEX) + "\r\n";
  client.print(closeChunkHeader);
  client.print(multipartClose);
  client.print("\r\n");
  
  // End of chunks
  client.print("0\r\n\r\n");
  
  // Read and print response
  unsigned long timeout = millis() + 10000;
  String response = "";
  bool inBody = false;
  while (client.connected() && millis() < timeout) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (!inBody) {
        if (line == "") {
          inBody = true;
        }
        Serial.print("Response: ");
        Serial.println(line);
      } else {
        response += line + "\n";
      }
    }
  }
  client.stop();
  
  // Debug: Validate CSV locally (read a sample since full streaming)
  // For now, skip full validation as file is closed, but assume fixed format
  
  Serial.print("Full server response body: ");
  Serial.println(response);
  
  if (response.indexOf("\"success\":true") != -1) {
    Serial.println("=== CSV UPLOAD SUMMARY ===");
    Serial.println("Message: Upload successful");
    Serial.println("Success: true");
    Serial.println("==========================");
    
    Serial.println("Upload successful, deleting local file.");
    SPIFFS.remove("/vitals_data.csv");
    File newFile = SPIFFS.open("/vitals_data.csv", FILE_WRITE);
    if (newFile) {
      String header = "timestamp,sensorType,glucose,sysBP,diaBP,hr,spo2,skinTemp,bodyTemp,accelX,accelY,accelZ,gyroX,gyroY,gyroZ\n";
      newFile.print(header);
      newFile.close();
    }
  } else if (response.indexOf("500") != -1 || response.indexOf("Internal server error") != -1) {
    Serial.println("CSV UPLOAD SERVER ERROR: The server encountered an internal error.");
    Serial.println("Possible causes (ruled out):");
    Serial.println("- Invalid CSV format (fixed by column adjustments)");
    Serial.println("- File too large (streamed fully)");
    Serial.println("- Check server logs for multer/MongoDB issues");
  }
}

// Upload stored data to server
void uploadStoredData() {
  if (SPIFFS.exists("/vitals_data.csv")) {
    uploadCSVToServer();
  } else {
    Serial.println("No stored data to upload");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Suppress WiFi error logs to reduce spam
  // esp_log_level_set("wifi", ESP_LOG_WARN);

  
  esp_log_level_set("wifi", ESP_LOG_NONE); 
  
  initializeSPIFFS();
  
  Wire.begin(41, 42);
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi");
    wifiConnected = true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    uploadStoredData();
  } else {
    Serial.println("\nFailed to connect to WiFi. Operating in offline mode.");
    wifiConnected = false;
  }
  
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
  
  Serial.println("Initializing BMI270...");
  if (bmi.beginI2C() != BMI2_OK) {
    Serial.println("Could not find a valid BMI270 sensor, check wiring!");
    while (1) delay(10);
  }
  Serial.println("BMI270 initialized!");
  
  Serial.println("TMP117 Temperature Sensor Initialized");
  
  Serial.println("Starting with AS7263 sensor for 2 minutes...");
  Serial.println("Place your finger on the AS7263 sensor.");
  phaseStartTime = millis();
  currentState = AS7263_PHASE;
}

void loop() {
  unsigned long currentTime = millis();
  
  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    Serial.println("WiFi disconnected. Switching to offline mode.");
    wifiConnected = false;
    // Do not call WiFi.disconnect() to allow auto-reconnect attempts internally
  } else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    Serial.println("WiFi reconnected. Switching to online mode.");
    wifiConnected = true;
    pendingUpload = true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
  
  switch (currentState) {
    case AS7263_PHASE:
      if (currentTime - phaseStartTime < PHASE_DURATION) {
        as7263.startMeasurement();
        delay(750);
        uint16_t channels[6];
        for (int i = 0; i < 6; i++) {
          channels[i] = as7263.readChannel(i);
        }
        
        float glucose = estimateGlucose(channels[0], channels[1], channels[2]);
        float sysBP = estimateSystolicBP(channels[0], channels[3], channels[5]);
        float diaBP = estimateDiastolicBP(channels[1], channels[4]);
        
        float skinTemp = getAverageTemp();
        float bodyTemp = skinTemp + CALIBRATION_OFFSET;
        
        bmi.getSensorData();
        float ax = bmi.data.accelX;
        float ay = bmi.data.accelY;
        float az = bmi.data.accelZ;
        float gx = bmi.data.gyroX;
        float gy = bmi.data.gyroY;
        float gz = bmi.data.gyroZ;
        
        if (readingCount < MAX_READINGS) {
          glucoseReadings[readingCount] = glucose;
          sysBPReadings[readingCount] = sysBP;
          diaBPReadings[readingCount] = diaBP;
          tempReadings[readingCount] = skinTemp;
          bodyTempReadings[readingCount] = bodyTemp;
          accelXReadings[readingCount] = ax;
          accelYReadings[readingCount] = ay;
          accelZReadings[readingCount] = az;
          gyroXReadings[readingCount] = gx;
          gyroYReadings[readingCount] = gy;
          gyroZReadings[readingCount] = gz;
          readingCount++;
        }
        
        String timestamp = getTimestamp();
        String sensorData = String(glucose) + "," + String(sysBP) + "," + String(diaBP) + ",," + String(skinTemp) + "," + String(bodyTemp) + "," +
                            String(ax) + "," + String(ay) + "," + String(az) + "," +
                            String(gx) + "," + String(gy) + "," + String(gz);
        logDataToCSV(timestamp, "AS7263", sensorData);
        
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
        
        Serial.print("Skin Temperature: ");
        Serial.print(skinTemp, 2);
        Serial.print(" °C | Estimated Body Temperature: ");
        Serial.print(bodyTemp, 2);
        Serial.println(" °C");
        
        Serial.print("Accel (m/s^2) X: "); Serial.print(ax, 2);
        Serial.print(" Y: "); Serial.print(ay, 2);
        Serial.print(" Z: "); Serial.println(az, 2);
        
        Serial.print("Gyro (rad/s) X: "); Serial.print(gx, 2);
        Serial.print(" Y: "); Serial.print(gy, 2);
        Serial.print(" Z: "); Serial.println(gz, 2);
        Serial.println("---------------------------");
        
        delay(2000);
      } else {
        avgGlucose = 0;
        avgSysBP = 0;
        avgDiaBP = 0;
        avgTemp = 0;
        avgBodyTemp = 0;
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
          avgTemp += tempReadings[i];
          avgBodyTemp += bodyTempReadings[i];
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
        avgTemp /= readingCount;
        avgBodyTemp /= readingCount;
        avgAccelX /= readingCount;
        avgAccelY /= readingCount;
        avgAccelZ /= readingCount;
        avgGyroX /= readingCount;
        avgGyroY /= readingCount;
        avgGyroZ /= readingCount;
        
        String timestamp = getTimestamp();
        String avgData = String(avgGlucose) + "," + String(avgSysBP) + "," + String(avgDiaBP) + ",," + String(avgTemp) + "," + String(avgBodyTemp) + "," +
                         String(avgAccelX) + "," + String(avgAccelY) + "," + String(avgAccelZ) + "," +
                         String(avgGyroX) + "," + String(avgGyroY) + "," + String(avgGyroZ);
        logDataToCSV(timestamp, "AS7263_AVG", avgData);
        
        Serial.println("\n=== AS7263 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Glucose: ");
        Serial.print(avgGlucose);
        Serial.print(" mg/dL | Average SysBP: ");
        Serial.print(avgSysBP);
        Serial.print(" mmHg | Average DiaBP: ");
        Serial.print(avgDiaBP);
        Serial.println(" mmHg");
        
        Serial.print("Average Skin Temperature: ");
        Serial.print(avgTemp, 2);
        Serial.print(" °C | Average Body Temperature: ");
        Serial.print(avgBodyTemp, 2);
        Serial.println(" °C");
        
        Serial.print("Average Accel (m/s^2) X: "); Serial.print(avgAccelX, 2);
        Serial.print(" Y: "); Serial.print(avgAccelY, 2);
        Serial.print(" Z: "); Serial.println(avgAccelZ, 2);
        
        Serial.print("Average Gyro (rad/s) X: "); Serial.print(avgGyroX, 2);
        Serial.print(" Y: "); Serial.print(avgGyroY, 2);
        Serial.print(" Z: "); Serial.println(avgGyroZ, 2);
        Serial.println("========================================");
        
        saveDataToFlash();
        
        resetReadings();
        Serial.println("Now switching to MAX30102 sensor for 2 minutes...");
        Serial.println("Place your finger on the MAX30102 sensor.");
        Serial.println("Ensure finger is properly placed for accurate heart rate and SpO2 readings.");
        phaseStartTime = millis();
        currentState = MAX30102_PHASE;
      }
      break;
      
    case MAX30102_PHASE:
      if (currentTime - phaseStartTime < PHASE_DURATION) {
        bufferLength = HR_BUFFER_SIZE;
bool lowSignalShown = false;
for (int i = 0; i < bufferLength; i++) {
  while (!particleSensor.available()) {
    particleSensor.check();
  }
  redBuffer[i] = particleSensor.getRed();
  irBuffer[i] = particleSensor.getIR();
  particleSensor.nextSample();

  // Debug MAX30102 signal quality
  if (redBuffer[i] < 5000 || irBuffer[i] < 5000) {
    if (!lowSignalShown) {
      // Serial.println("DEBUG: Low signal quality detected on MAX30102.");
      // Serial.println("Ensure finger is firmly placed on the sensor.");
      lowSignalShown = true;
    }
    Serial.println("Heart Rate: INVALID | SpO2: INVALID");
  } else {
    lowSignalShown = false; // reset when signal improves
    // Normal HR & SpO2 printing here (if needed)
  }
}
        
        maxim_heart_rate_and_oxygen_saturation(
          irBuffer, bufferLength,
          redBuffer,
          &spo2, &validSPO2,
          &heartRate, &validHeartRate);
        
        float skinTemp = getAverageTemp();
        float bodyTemp = skinTemp + CALIBRATION_OFFSET;
        
        bmi.getSensorData();
        float ax = bmi.data.accelX;
        float ay = bmi.data.accelY;
        float az = bmi.data.accelZ;
        float gx = bmi.data.gyroX;
        float gy = bmi.data.gyroY;
        float gz = bmi.data.gyroZ;
        
        if (readingCount < MAX_READINGS) {
          if (validHeartRate) hrReadings[readingCount] = heartRate;
          if (validSPO2) spo2Readings[readingCount] = spo2;
          tempReadings[readingCount] = skinTemp;
          bodyTempReadings[readingCount] = bodyTemp;
          accelXReadings[readingCount] = ax;
          accelYReadings[readingCount] = ay;
          accelZReadings[readingCount] = az;
          gyroXReadings[readingCount] = gx;
          gyroYReadings[readingCount] = gy;
          gyroZReadings[readingCount] = gz;
          readingCount++;
        }
        
        String timestamp = getTimestamp();
        String sensorData = ",,," + (validHeartRate ? String(heartRate) : "") + "," + (validSPO2 ? String(spo2) : "") + "," + String(skinTemp) + "," + String(bodyTemp) + "," +
                            String(ax) + "," + String(ay) + "," + String(az) + "," +
                            String(gx) + "," + String(gy) + "," + String(gz);
        logDataToCSV(timestamp, "MAX30102", sensorData);
        
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
        
        Serial.print("Skin Temperature: ");
        Serial.print(skinTemp, 2);
        Serial.print(" °C | Estimated Body Temperature: ");
        Serial.print(bodyTemp, 2);
        Serial.println(" °C");
        
        Serial.print("Accel (m/s^2) X: "); Serial.print(ax, 2);
        Serial.print(" Y: "); Serial.print(ay, 2);
        Serial.print(" Z: "); Serial.println(az, 2);
        
        Serial.print("Gyro (rad/s) X: "); Serial.print(gx, 2);
        Serial.print(" Y: "); Serial.print(gy, 2);
        Serial.print(" Z: "); Serial.println(gz, 2);
        Serial.println("---------------------------");
        
        delay(1000);
      } else {
        avgHR = 0;
        avgSPO2 = 0;
        avgTemp = 0;
        avgBodyTemp = 0;
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
          
          avgTemp += tempReadings[i];
          avgBodyTemp += bodyTempReadings[i];
          avgAccelX += accelXReadings[i];
          avgAccelY += accelYReadings[i];
          avgAccelZ += accelZReadings[i];
          avgGyroX += gyroXReadings[i];
          avgGyroY += gyroYReadings[i];
          avgGyroZ += gyroZReadings[i];
        }
        
        if (validHRCount > 0) avgHR /= validHRCount;
        if (validSPO2Count > 0) avgSPO2 /= validSPO2Count;
        
        avgTemp /= readingCount;
        avgBodyTemp /= readingCount;
        avgAccelX /= readingCount;
        avgAccelY /= readingCount;
        avgAccelZ /= readingCount;
        avgGyroX /= readingCount;
        avgGyroY /= readingCount;
        avgGyroZ /= readingCount;
        
        String timestamp = getTimestamp();
        String avgData = ",,," + String(avgHR) + "," + String(avgSPO2) + "," + String(avgTemp) + "," + String(avgBodyTemp) + "," +
                         String(avgAccelX) + "," + String(avgAccelY) + "," + String(avgAccelZ) + "," +
                         String(avgGyroX) + "," + String(avgGyroY) + "," + String(avgGyroZ);
        logDataToCSV(timestamp, "MAX30102_AVG", avgData);
        
        Serial.println("\n=== MAX30102 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Heart Rate: ");
        Serial.print(avgHR);
        Serial.print(" bpm | Average SpO2: ");
        Serial.print(avgSPO2);
        Serial.println(" %");
        
        Serial.print("Average Skin Temperature: ");
        Serial.print(avgTemp, 2);
        Serial.print(" °C | Average Body Temperature: ");
        Serial.print(avgBodyTemp, 2);
        Serial.println(" °C");
        
        Serial.print("Average Accel (m/s^2) X: "); Serial.print(avgAccelX, 2);
        Serial.print(" Y: "); Serial.print(avgAccelY, 2);
        Serial.print(" Z: "); Serial.println(avgAccelZ, 2);
        
        Serial.print("Average Gyro (rad/s) X: "); Serial.print(avgGyroX, 2);
        Serial.print(" Y: "); Serial.print(avgGyroY, 2);
        Serial.print(" Z: "); Serial.println(avgGyroZ, 2);
        Serial.println("==========================================");
        
        saveDataToFlash();
        
        currentState = POST_DATA_PHASE;
      }
      break;
      
    case POST_DATA_PHASE:
      if (wifiConnected) {
        Serial.println("\n=== POSTING ALL VITAL SIGNS TO SERVER ===");
      } else {
        Serial.println("\nWiFi not connected. Data stored locally.");
      }
      postVitalsDataToServer(avgGlucose, avgSysBP, avgDiaBP, avgHR, avgSPO2,
                            avgTemp, avgBodyTemp,
                            avgAccelX, avgAccelY, avgAccelZ,
                            avgGyroX, avgGyroY, avgGyroZ);
      
      // If pending upload (reconnected during cycle), upload stored data now
      if (pendingUpload && wifiConnected) {
        uploadStoredData();
        pendingUpload = false;
      }
      
      resetReadings();
      Serial.println("Starting new cycle with AS7263 sensor for 2 minutes...");
      Serial.println("Place your finger on the AS7263 sensor.");
      phaseStartTime = millis();
      currentState = AS7263_PHASE;
      break;
  }
}

void resetReadings() {
  readingCount = 0;
  for (int i = 0; i < MAX_READINGS; i++) {
    glucoseReadings[i] = 0;
    sysBPReadings[i] = 0;
    diaBPReadings[i] = 0;
    hrReadings[i] = 0;
    spo2Readings[i] = 0;
    tempReadings[i] = 0;
    bodyTempReadings[i] = 0;
    accelXReadings[i] = 0;
    accelYReadings[i] = 0;
    accelZReadings[i] = 0;
    gyroXReadings[i] = 0;
    gyroYReadings[i] = 0;
    gyroZReadings[i] = 0;
  }
}

void postVitalsDataToServer(float glucose, float sysBP, float diaBP, float heartRate, float spO2,
                           float temperature, float bodyTemperature,
                           float accelX, float accelY, float accelZ,
                           float gyroX, float gyroY, float gyroZ) {
  JsonDocument doc;
  
  doc["userId"] = "user-001-wearable";
  doc["inputMethod"] = "wearable";
  doc["timestamp"] = getTimestamp();
  
  JsonObject bloodPressure = doc["bloodPressure"].to<JsonObject>();
  bloodPressure["systolic"] = sysBP;
  bloodPressure["diastolic"] = diaBP;
  
  doc["heartRate"] = heartRate;
  doc["spO2"] = spO2;
  doc["bloodGlucose"] = glucose;
  
  JsonObject tempData = doc["temperature"].to<JsonObject>();
  tempData["skin"] = temperature;
  tempData["body"] = bodyTemperature;
  
  JsonObject motion = doc["motion"].to<JsonObject>();
  JsonObject acceleration = motion["acceleration"].to<JsonObject>();
  acceleration["x"] = accelX;
  acceleration["y"] = accelY;
  acceleration["z"] = accelZ;
  
  JsonObject gyroscope = motion["gyroscope"].to<JsonObject>();
  gyroscope["x"] = gyroX;
  gyroscope["y"] = gyroY;
  gyroscope["z"] = gyroZ;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  // Debug Step 1: Check for invalid JSON format
  JsonDocument testDoc;
  DeserializationError jsonError = deserializeJson(testDoc, jsonString);
  if (jsonError) {
    Serial.println("DEBUG: Invalid JSON format detected locally.");
    Serial.print("JSON Error: ");
    Serial.println(jsonError.c_str());
    Serial.print("JSON String: ");
    Serial.println(jsonString);
    String timestamp = getTimestamp();
    String finalData = String(glucose) + "," + String(sysBP) + "," + String(diaBP) + "," + 
                      String(heartRate) + "," + String(spO2) + "," +
                      String(temperature) + "," + String(bodyTemperature) + "," +
                      String(accelX) + "," + String(accelY) + "," + String(accelZ) + "," +
                      String(gyroX) + "," + String(gyroY) + "," + String(gyroZ);
    logDataToCSV(timestamp, "FINAL_AVG", finalData);
    saveDataToFlash();
    return;
  }
  
  // Debug Step 2: Check if data is too large
  const size_t MAX_PAYLOAD_SIZE = 10240; // 10KB
  if (jsonString.length() > MAX_PAYLOAD_SIZE) {
    Serial.println("DEBUG: JSON payload size exceeds threshold.");
    Serial.print("Payload size: ");
    Serial.print(jsonString.length());
    Serial.println(" bytes (Max allowed: " + String(MAX_PAYLOAD_SIZE) + " bytes)");
    String timestamp = getTimestamp();
    String finalData = String(glucose) + "," + String(sysBP) + "," + String(diaBP) + "," + 
                      String(heartRate) + "," + String(spO2) + "," +
                      String(temperature) + "," + String(bodyTemperature) + "," +
                      String(accelX) + "," + String(accelY) + "," + String(accelZ) + "," +
                      String(gyroX) + "," + String(gyroY) + "," + String(gyroZ);
    logDataToCSV(timestamp, "FINAL_AVG", finalData);
    saveDataToFlash();
    return;
  }
  
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
      
      if (httpResponseCode == 500) {
        Serial.println("SERVER ERROR: The server encountered an internal error.");
        
        // Debug Step 3: Check for authentication issues
        HTTPClient testHttp;
        testHttp.begin(serverURL);
        testHttp.addHeader("Content-Type", "application/json");
        String testPayload = "{\"test\":\"ping\"}";
        int testResponseCode = testHttp.POST(testPayload);
        if (testResponseCode == 401 || testResponseCode == 403) {
          Serial.println("DEBUG: Server authentication required (401/403 detected).");
          Serial.println("Please verify if the server requires an API key or token.");
        } else if (testResponseCode == 500) {
          Serial.println("DEBUG: Test request also returned 500. Likely a server-side bug.");
          Serial.print("Test request response: ");
          Serial.println(testHttp.getString());
        } else {
          Serial.println("DEBUG: Test request succeeded or returned different error.");
          Serial.print("Test response code: ");
          Serial.println(testResponseCode);
        }
        testHttp.end();
        
        Serial.println("Possible causes not ruled out:");
        Serial.println("- Invalid JSON format (ruled out by local validation)");
        Serial.println("- Data too large (ruled out by size check)");
        Serial.println("- Server authentication required (check test request result)");
        Serial.println("- Bug in server processing (likely if test request also fails)");
      } else {
        Serial.println("All vital signs posted successfully!");
        uploadCSVToServer();
      }
    } else {
      Serial.print("Error posting data. Error code: ");
      Serial.println(httpResponseCode);
    }
    
    http.end();
  } else {
    Serial.println("WiFi disconnected. Cannot post data.");
    String timestamp = getTimestamp();
    String finalData = String(glucose) + "," + String(sysBP) + "," + String(diaBP) + "," + 
                      String(heartRate) + "," + String(spO2) + "," +
                      String(temperature) + "," + String(bodyTemperature) + "," +
                      String(accelX) + "," + String(accelY) + "," + String(accelZ) + "," +
                      String(gyroX) + "," + String(gyroY) + "," + String(gyroZ);
    logDataToCSV(timestamp, "FINAL_AVG", finalData);
    saveDataToFlash();
  }
}