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

// Operation states
enum OperationState {
  AS7263_PHASE,
  MAX30102_PHASE,
  CONTINUOUS_PHASE
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
int readingCount = 0;

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
        
        // Store readings for averaging
        if (readingCount < MAX_READINGS) {
          glucoseReadings[readingCount] = glucose;
          sysBPReadings[readingCount] = sysBP;
          diaBPReadings[readingCount] = diaBP;
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
        
        delay(2000); // Wait 2 seconds between readings
      } else {
        // 2 minutes have passed - calculate and display averages
        float avgGlucose = 0, avgSysBP = 0, avgDiaBP = 0;
        
        for (int i = 0; i < readingCount; i++) {
          avgGlucose += glucoseReadings[i];
          avgSysBP += sysBPReadings[i];
          avgDiaBP += diaBPReadings[i];
        }
        
        avgGlucose /= readingCount;
        avgSysBP /= readingCount;
        avgDiaBP /= readingCount;
        
        Serial.println("\n=== AS7263 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Glucose: ");
        Serial.print(avgGlucose);
        Serial.print(" mg/dL | Average SysBP: ");
        Serial.print(avgSysBP);
        Serial.print(" mmHg | Average DiaBP: ");
        Serial.print(avgDiaBP);
        Serial.println(" mmHg");
        Serial.println("========================================");
        
        // Reset for next phase
        readingCount = 0;
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
        
        // Store readings for averaging
        if (readingCount < MAX_READINGS) {
          if (validHeartRate) hrReadings[readingCount] = heartRate;
          if (validSPO2) spo2Readings[readingCount] = spo2;
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
        
        delay(1000); // Wait 1 second between readings
      } else {
        // 2 minutes have passed - calculate and display averages
        float avgHR = 0, avgSPO2 = 0;
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
        }
        
        if (validHRCount > 0) avgHR /= validHRCount;
        if (validSPO2Count > 0) avgSPO2 /= validSPO2Count;
        
        Serial.println("\n=== MAX30102 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Heart Rate: ");
        Serial.print(avgHR);
        Serial.print(" bpm | Average SpO2: ");
        Serial.print(avgSPO2);
        Serial.println(" %");
        Serial.println("==========================================");
        
        // Reset for next phase
        readingCount = 0;
        Serial.println("Now continuing with MAX30102 sensor only...");
        currentState = CONTINUOUS_PHASE;
      }
      break;
      
    case CONTINUOUS_PHASE:
      // Continuous phase: use only MAX30102
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
      
      delay(1000); // Wait 1 second between readings
      break;
  }
}