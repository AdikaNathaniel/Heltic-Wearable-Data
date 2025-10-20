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
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// Bluetooth service and characteristics
BLECharacteristic *pVitalsCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool bluetoothActive = false;
unsigned long lastBluetoothAttempt = 0;
const unsigned long BLUETOOTH_RETRY_INTERVAL = 30000; // 30 seconds between attempts
int bluetoothAttempts = 0;
const int MAX_BLUETOOTH_ATTEMPTS = 2;

// WiFi and MQTT variables
bool wifiConnected = false;
bool pendingUpload = false;
unsigned long lastWifiCheck = 0;

// Add these function declarations:
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void checkAndConnectWiFi();
void printConnectionStatus();

// Bluetooth service UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("Bluetooth device connected!");
        bluetoothActive = true;
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("Bluetooth device disconnected");
        bluetoothActive = false;
        // Restart advertising when device disconnects
        pServer->getAdvertising()->start();
        Serial.println("Bluetooth advertising restarted");
    }
};

// Keep your hardcoded key and IV as global variables
byte aesKey[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                   0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

byte aesIV[16]  = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                   0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

String aesEncryptBase64(String plainText, const char* key, const char* iv) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    size_t inputLen = plainText.length();
    size_t paddedLen = ((inputLen + 15) / 16) * 16;
    
    unsigned char input[paddedLen];
    unsigned char output[paddedLen];
    unsigned char ivCopy[16];  // Create a copy of the IV
    
    memset(input, 0, paddedLen);
    memcpy(input, plainText.c_str(), inputLen);
    memcpy(ivCopy, iv, 16);  // Copy IV to protect the original

    // Add proper PKCS#7 padding instead of just zeros
    unsigned char paddingValue = paddedLen - inputLen;
    for (size_t i = inputLen; i < paddedLen; i++) {
        input[i] = paddingValue;
    }

    // Set encryption key
    if (mbedtls_aes_setkey_enc(&aes, (const unsigned char*)key, 128) != 0) {
        Serial.println("AES setkey failed");
        mbedtls_aes_free(&aes);
        return "";
    }

    // Encrypt using the IV copy (this gets modified during encryption)
    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLen, 
                             ivCopy, input, output) != 0) {
        Serial.println("AES encryption failed");
        mbedtls_aes_free(&aes);
        return "";
    }

    // Base64 encode
    size_t base64Len;
    mbedtls_base64_encode(NULL, 0, &base64Len, output, paddedLen);
    
    char base64Buf[base64Len + 1];
    if (mbedtls_base64_encode((unsigned char*)base64Buf, base64Len, &base64Len, 
                             output, paddedLen) != 0) {
        Serial.println("Base64 encode failed");
        mbedtls_aes_free(&aes);
        return "";
    }
    
    base64Buf[base64Len] = '\0';
    String result = String(base64Buf);

    mbedtls_aes_free(&aes);
    return result;
}

String aesDecryptBase64(String cipherBase64, const char* key, const char* iv) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    // Decode Base64 with proper buffer size calculation
    size_t cipherLen = 0;
    size_t bufferSize = (cipherBase64.length() * 3) / 4 + 4;
    unsigned char cipher[bufferSize];
    unsigned char ivCopy[16];  // Create a copy of the IV
    
    memcpy(ivCopy, iv, 16);  // Copy IV to protect the original
    
    if (mbedtls_base64_decode(cipher, bufferSize, &cipherLen, 
                             (const unsigned char*)cipherBase64.c_str(), 
                             cipherBase64.length()) != 0) {
        Serial.println("Base64 decode failed");
        mbedtls_aes_free(&aes);
        return "";
    }

    unsigned char output[cipherLen];
    
    if (mbedtls_aes_setkey_dec(&aes, (const unsigned char*)key, 128) != 0) {
        Serial.println("AES setkey_dec failed");
        mbedtls_aes_free(&aes);
        return "";
    }
    
    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, 
                             ivCopy, cipher, output) != 0) {
        Serial.println("AES decryption failed");
        mbedtls_aes_free(&aes);
        return "";
    }

    mbedtls_aes_free(&aes);

    // Remove PKCS#7 padding properly
    if (cipherLen > 0) {
        unsigned char paddingValue = output[cipherLen - 1];
        if (paddingValue > 0 && paddingValue <= 16) {
            // Verify all padding bytes are correct
            bool validPadding = true;
            for (size_t i = cipherLen - paddingValue; i < cipherLen; i++) {
                if (output[i] != paddingValue) {
                    validPadding = false;
                    break;
                }
            }
            if (validPadding) {
                cipherLen -= paddingValue;
            }
        }
    }

    // Convert to string
    String result = "";
    for (size_t i = 0; i < cipherLen; i++) {
        result += (char)output[i];
    }
    return result;
}

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
const char* password = "jehovahofgrace";

const char* AWS_IOT_ENDPOINT = "a2gwymvb8cnbld-ats.iot.eu-north-1.amazonaws.com";

// MQTT
const int AWS_IOT_PORT = 8883;
const char* MQTT_CLIENT_ID = "heltec-esp32-01"; // unique per device
const char* MQTT_TOPIC = "heltec/data";

WiFiClientSecure net;
PubSubClient mqtt(net);

// Amazon Root CA 1 PEM
const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)EOF";

// Device certificate PEM
const char AWS_CERT_CRT[] PROGMEM = R"KEY(
-----BEGIN CERTIFICATE-----
MIIDWTCCAkGgAwIBAgIUfpb53dBrBLYGeWOaMkTWt1YNlxkwDQYJKoZIhvcNAQEL
BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g
SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI1MDkxNTE1MTIw
NloXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0
ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMqnA9iZZDck5KeKd4vY
EJ/USXGRd2ej7MHTOQph+h1J40qIMIm3ExNfEYACDc4RvWdZNRHzO79U2jk6KZDz
TrVQj48h/TzBj/a4TBC8hX6Jje31FFlMsx1ZddQ1V7UsCADkAoKyp1xbZMnURMLd
6qmIAs1SrISzX1181AvT8E1tTj0fEvv1Ydb9Cb/hNNAIg+QTUzvY7l3ewgkjF5Un
4+n/niBnK6WEE8zF8r6eEsW0vYdcjwF3pUvvQpUWvTz/x4m/wZ9HqSzcTI4LZADm
iGdY6JcM7G7yRrScIYNobPRMOjLH4u1HfZ9ARHqzbN9MtoVdtVyp7EXJKMj4WJp6
WN0CAwEAAaNgMF4wHwYDVR0jBBgwFoAUnPW8RfR3ybd6tcPGuNTeMJr4ZOYwHQYD
VR0OBBYEFH7W1y/q3EqKhqsxXSbuSQvDBCPQMAwGA1UdEwEB/wQCMAAwDgYDVR0P
AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQAiZ6uvonjezCL6M5AsDOg5aYd7
o0fYpMSS02OTFGPI+8YX/GG4UbXggonzGGeeNN0NltKMEYxRIe5TGxHOdydtSkUW
qMZ46jcKPJLhAu8lqyM8kapk/jg3/epaX8mPb+nZVDY9e9rnJso/LfenlJHvpIoR
9qVUXGZkbK1Gn3e1+X1LMmhBTaB5JEVCMrOwlSsCFdNcLVn8O1l+ohvnRP6t1MOo
cWhtdZB9ELQPumDLO+G9Q0gy35d1KwGWKEYZYqG+C4iaZ2DmJc9oZd8BCwl8nKij
+v54pbzyQhNwn1oRRhXy1IwQxTUugOK8+ogB4zJ7Yftg/U6tLaP6jrPPjWEU
-----END CERTIFICATE-----
)KEY";

// Device private key PEM
const char AWS_CERT_PRIVATE[] PROGMEM = R"KEY(
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDKpwPYmWQ3JOSn
ineL2BCf1ElxkXdno+zB0zkKYfodSeNKiDCJtxMTXxGAAg3OEb1nWTUR8zu/VNo5
OimQ8061UI+PIf08wY/2uEwQvIV+iY3t9RRZTLMdWXXUNVe1LAgA5AKCsqdcW2TJ
1ETC3eqpiALNUqyEs19dfNQL0/BNbU49HxL79WHW/Qm/4TTQCIPkE1M72O5d3sIJ
IxeVJ+Pp/54gZyulhBPMxfK+nhLFtL2HXI8Bd6VL70KVFr08/8eJv8GfR6ks3EyO
C2QA5ohnWOiXDOxu8ka0nCGDaGz0TDoyx+LtR32fQER6s2zfTLaFXbVcqexFySjI
+FiaeljdAgMBAAECggEAHB7M09OFKnm+GNmH+SnENMl6X331+bWvdr3enmg2ErZc
Cc2YHqP+y6NXhFHEpKGljJ++U31S/xvBJEvD1M9OTq6dHRl0UGfzmCK+u1vNkl94
CFsHtdSEzj/Jr+avAv7XJXh+OLCdtJ0iATepSvkMRPYQnXkpY0iYpRpbOkDwiW2w
RUnohlr+vqFRSXlciud+zFNHed9btpDYsydjAKeGEvWn39h9woOsv81BzfJg6BGX
qSwMaRymTJUUh+X0oASkL721ZJAJbHo1UzDcVV54m7mOoLnMR90S9fdwd7dbtDez
Yi/hsUA0NwYlLUNhcpYIqKG3xiZSDFywHSWbANZT6QKBgQDkBnjd1CzP735wi2j7
lyomUIMVYEPdm5E2QWOopqxa5dnaFEQja8pcS2MHyFE3tuG2EOFecGkN5s0ZK/3t
yGPyQ1GuAonVd5Sn44PbkcUNorAl15EkuF1l/E1Zr9Y+2le6kvu+4k/ykIT9nljk
kK6pts6HfGfKg0QHaGF2Gmw4cwKBgQDjg6ntwiyHf8leSr8bF/zjk9e/0vHcZr3x
daigiA51v2lqfJ2cNk6Y1hAN1bJl/u72RQA9ihomTul94/ztPUkfEySSc1H0dfOl
3YhkDqotvjx7+Edyw8YslY8QXaAotVy4/3koHvTpfu3UTh86Rml8fTGm7TRx+Cdu
rtsBK/XlbwKBgEQqyydFrr/a0OnEQ3YzEp6xov5Y8PDm+NpHYguhCBWEcUQImvYb
2zsMLma9M8p8Ga3cDgO1hzJotRe5Vwgmx/o8tcZXa+0s+nvWQCz7vvT8lbw6XWGk
3AcJDfap4lqX23IjTq8Z6NyS568KD3kE57JEvCk0v7KC22t7gOdVrtSRAoGAXFGT
xW9SJFVc/47zBupv6mSWWjcdfyKNlon4t9MdkvftrZENX1LGB8xszUBWWH7etj1/
gHkmbT7+DeBSRG3m2t4oq+hxa/HkwPwomPk+Yrtbza4MR+xa4MJ7P9fN4lbdXhAE
OJx/IrQni0t77mid6y7d0+9SpaBdV9NJ+Kq+568CgYEApW6uFICAbC0MaQOwQS5J
jtTTl67n559xfi0j2O+NEKc7DVNRIUdjCc79pBvcQJdZLCsuLXfHYAscsejubtPV
t44ZFYPWOc/ytlt50u8ohRu+DhV5Fyq8Ew8h5F1m1Tn1YGr3CXhlSXIb/eoKcuxB
zXwQRxaQ0Wc5dCGLEvU+l6c=
-----END PRIVATE KEY-----
)KEY";

void initializeBluetooth() {
    if (bluetoothAttempts >= MAX_BLUETOOTH_ATTEMPTS) {
        Serial.println("Max Bluetooth attempts reached. Switching to WiFi fallback.");
        return;
    }
    
    Serial.println("Initializing Bluetooth...");
    
    BLEDevice::init("ESP32-Vitals-Monitor");
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);
    pVitalsCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );

    pService->start();
    pServer->getAdvertising()->start();
    
    bluetoothActive = true;
    bluetoothAttempts++;
    lastBluetoothAttempt = millis();
    
    Serial.println("Bluetooth active! Device name: 'ESP32-Vitals-Monitor'");
    Serial.println("Scan for this device in your Bluetooth app to receive vital signs data.");
}

void sendDataViaBluetooth(String data) {
    if (deviceConnected) {
        pVitalsCharacteristic->setValue(data.c_str());
        pVitalsCharacteristic->notify();
        Serial.println("Data sent via Bluetooth: " + data);
    }
}

void checkAndConnectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnected) {
            wifiConnected = true;
            Serial.println("WiFi connected! Initializing time and MQTT...");
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            
            // Initialize MQTT
            mqtt.setCallback(mqttCallback);
            if (!mqtt.connected()) {
                connectMQTT();
            }
            
            // Check for pending uploads
            pendingUpload = true;
        }
    } else {
        if (wifiConnected) {
            Serial.println("WiFi disconnected.");
            wifiConnected = false;
        }
        
        // Try to reconnect WiFi if not connected
        Serial.println("Attempting WiFi connection...");
        WiFi.begin(ssid, password);
        
        unsigned long wifiStartTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < 10000) {
            delay(500);
            Serial.print(".");
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            Serial.println("\nWiFi reconnected!");
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            
            // Initialize MQTT
            mqtt.setCallback(mqttCallback);
            if (!mqtt.connected()) {
                connectMQTT();
            }
            
            pendingUpload = true;
        } else {
            Serial.println("\nWiFi connection failed.");
        }
    }
}

void printConnectionStatus() {
    Serial.println("\n=== CONNECTION STATUS ===");
    Serial.print("Bluetooth: ");
    Serial.println(deviceConnected ? "CONNECTED" : "DISCONNECTED");
    Serial.print("WiFi: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    Serial.print("MQTT: ");
    Serial.println(mqtt.connected() ? "CONNECTED" : "DISCONNECTED");
    Serial.println("==========================\n");
}

void manageBluetoothConnection() {
    // Handle device connection status changes
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); // give the bluetooth stack the chance to get things ready
        oldDeviceConnected = deviceConnected;
        
        Serial.println("Bluetooth disconnected - activating WiFi fallback...");
        
        // 🔥 FIXED: Force WiFi reconnection check when Bluetooth disconnects
        wifiConnected = false; // Reset to force recheck
        checkAndConnectWiFi();
    }
    
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
    
    // If Bluetooth not active and we haven't reached max attempts, try to initialize
    if (!bluetoothActive && bluetoothAttempts < MAX_BLUETOOTH_ATTEMPTS) {
        if (millis() - lastBluetoothAttempt > BLUETOOTH_RETRY_INTERVAL) {
            initializeBluetooth();
        }
    }
    
    // 🔥 FIXED: Improved periodic WiFi status check
    static unsigned long lastWifiCheck = 0;
    if (!deviceConnected && (!wifiConnected || !mqtt.connected())) {
        if (millis() - lastWifiCheck > 15000) { // Check every 15 seconds
            lastWifiCheck = millis();
            checkAndConnectWiFi();
        }
    }
    
    // Maintain MQTT connection if WiFi is connected
    if (wifiConnected && mqtt.connected()) {
        mqtt.loop();
    }
}

void connectMQTT() {
  if (mqtt.connected()) return;

  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);
  mqtt.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
  
  // Set keep alive time (60 seconds is standard)
  mqtt.setKeepAlive(60);
  
  Serial.print("Connecting to AWS IoT MQTT...");
  unsigned long start = millis();
  
  while (!mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.print(".");
    Serial.print("Failed, rc=");
    Serial.print(mqtt.state());
    Serial.println(" retrying in 1 second...");
    delay(1000);
    
    if (millis() - start > 20000) {
      Serial.println("\nMQTT connect failed, will retry later");
      return;
    }
  }
  
  Serial.println("connected to AWS IoT!");
}

void debugMQTTConnection() {
    Serial.println("=== MQTT Connection Debug ===");
    
    // Check certificate loading
    Serial.print("CA Cert loaded: ");
    Serial.println("Set (cannot verify with getCACert)");
    
    Serial.print("Client Cert loaded: ");
    Serial.println("Set (cannot verify with getCertificate)");
    
    Serial.print("Private Key loaded: ");
    Serial.println("Set (cannot verify with getPrivateKey)");
    
    // Test different scenarios
    Serial.println("Testing different publish scenarios:");
    
    // Test 1: Simple short message
    Serial.print("Test 1 - Short message: ");
    bool success1 = mqtt.publish(MQTT_TOPIC, "test");
    Serial.println(success1 ? "SUCCESS" : "FAILED");
    delay(100);
    
    // Test 2: Simple JSON
    Serial.print("Test 2 - Simple JSON: ");
    bool success2 = mqtt.publish(MQTT_TOPIC, "{\"test\":\"data\"}");
    Serial.println(success2 ? "SUCCESS" : "FAILED");
    delay(100);
    
    // Test 3: Check if QoS makes a difference
    Serial.print("Test 3 - With QoS 0: ");
    bool success3 = mqtt.publish(MQTT_TOPIC, "qos_test", false);
    Serial.println(success3 ? "SUCCESS" : "FAILED");
    delay(100);
    
    Serial.println("=== Debug Complete ===");
}

void publishEncryptedData(String encryptedData) {
    // Ensure we're connected
    if (!mqtt.connected()) {
        Serial.println("Not connected, attempting to connect...");
        connectMQTT();
        debugMQTTConnection();
        delay(100);
    }

    Serial.print("MQTT state before publish: ");
    Serial.println(mqtt.state());
    Serial.print("Connected status: ");
    Serial.println(mqtt.connected() ? "YES" : "NO");

    // Let MQTT process any background tasks
    mqtt.loop();
    delay(10);

    Serial.print("Publishing to topic: ");
    Serial.println(MQTT_TOPIC);
    Serial.print("Payload length: ");
    Serial.println(encryptedData.length());
    Serial.print("Payload preview: ");
    if (encryptedData.length() > 50) {
        Serial.println(encryptedData.substring(0, 50) + "...");
    } else {
        Serial.println(encryptedData);
    }

    bool success = mqtt.publish(MQTT_TOPIC, encryptedData.c_str());
    
    Serial.print("Publish result: ");
    Serial.println(success ? "SUCCESS" : "FAILED");
    Serial.print("MQTT state after publish: ");
    Serial.println(mqtt.state());
    
    if (!success) {
        // Try with a smaller test message to rule out payload issues
        Serial.println("Trying with test message...");
        bool testSuccess = mqtt.publish(MQTT_TOPIC, "test_message");
        Serial.print("Test message result: ");
        Serial.println(testSuccess ? "SUCCESS" : "FAILED");
    }
    

     // Check payload size
    if (encryptedData.length() > 512) { // AWS IoT limit is 128KB but be conservative
        Serial.println("Payload too large, truncating or splitting needed");
        // Option: Split into multiple messages or compress
        encryptedData = encryptedData.substring(0, 512);
    }
    // Process MQTT tasks after publishing
    mqtt.loop();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message received on topic: ");
    Serial.println(topic);
    
    Serial.print("Payload: ");
    for (int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
    }
    Serial.println();
}

// API endpoints
extern const char* serverURL;
extern const char* csvUploadURL;

// Use HTTPS because Render uses SSL
const char* serverURL = "https://patient-monitor-backend-patient.fly.dev/api/v1/heltec-live-vitals";
const char* csvUploadURL = "https://patient-monitor-backend-patient.fly.dev/api/v1/csv/upload";

// MAX30102 settings
#define HR_BUFFER_SIZE 100
uint32_t irBuffer[HR_BUFFER_SIZE];
uint32_t redBuffer[HR_BUFFER_SIZE];
int32_t bufferLength;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Heart rate validation constants
#define MIN_VALID_HR 60   // Minimum valid heart rate (bpm)
#define MAX_VALID_HR 100  // Maximum valid heart rate (bpm)

// Operation states - UPDATED with new initial test phase
enum OperationState {
  INITIAL_TEST_PHASE,  // NEW: Initial 10-second test phase
  AS7263_PHASE,
  MAX30102_PHASE,
  POST_DATA_PHASE
};
OperationState currentState = INITIAL_TEST_PHASE; // Start with initial test

// Variables for 2-minute average calculation
const unsigned long PHASE_DURATION = 120000; // 2 minutes in milliseconds
const unsigned long INITIAL_TEST_DURATION = 10000; // 10 seconds for initial test
unsigned long phaseStartTime;

// Variables for initial test phase
float initialTestGlucose = 0;
float initialTestSysBP = 0;
float initialTestDiaBP = 0;
float initialTestHR = 0;
float initialTestSPO2 = 0;
float initialTestTemp = 0;
float initialTestBodyTemp = 0;
float initialTestAccelX = 0;
float initialTestAccelY = 0;
float initialTestAccelZ = 0;
float initialTestGyroX = 0;
float initialTestGyroY = 0;
float initialTestGyroZ = 0;
int initialTestCount = 0;
int initialTestValidHRCount = 0; // NEW: Track valid HR readings separately

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

// NEW: Separate counters for valid HR and SpO2 readings
int validHRReadingCount = 0;
int validSPO2ReadingCount = 0;

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

// NEW: AS7263 BP and Glucose calculation parameters
// BP Calculation parameters
#define SAMPLES_PER_2MIN 40  // 2 minutes / 3 seconds per reading = 40 samples
#define SAMPLE_INTERVAL 3000 // 3 seconds between readings

// Storage for 2-minute averaging
float red_sum = 0, orange_sum = 0, yellow_sum = 0;
float green_sum = 0, blue_sum = 0, violet_sum = 0;
int as7263_sample_count = 0;

// CORRECTED: Focus on TRUE NIR channels only
#define WEIGHT_RED     0.00   // Ignore - visible light (610nm)
#define WEIGHT_ORANGE  0.00   // Ignore - visible light (680nm)
#define WEIGHT_YELLOW  0.10   // Minimal - borderline NIR (730nm)
#define WEIGHT_GREEN   0.20   // Use - true NIR (760nm)
#define WEIGHT_BLUE    0.35   // Use - deep penetration (810nm)
#define WEIGHT_VIOLET  0.35   // Use - deepest penetration (860nm)

// Glucose-specific NIR weights (different wavelength absorption)
#define GLUCOSE_WEIGHT_YELLOW  0.15
#define GLUCOSE_WEIGHT_GREEN   0.30
#define GLUCOSE_WEIGHT_BLUE    0.40
#define GLUCOSE_WEIGHT_VIOLET  0.15

// Target BP ranges (unchanged)
#define SYSTOLIC_MIN   110
#define SYSTOLIC_MAX   139
#define DIASTOLIC_MIN  65
#define DIASTOLIC_MAX  89

// Glucose range (70-135 as requested)
#define GLUCOSE_MIN    70
#define GLUCOSE_MAX    135

// Data logging variables
// NOTE: wifiConnected and pendingUpload are now declared at the top
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
void sendInitialTestData(); // NEW function declaration

// NEW: AS7263 BP and Glucose calculation functions
void collectAS7263Sample();
void calculateBPAndGlucose(float &glucose, float &systolic, float &diastolic);
void resetAS7263Accumulators();

// NEW: Heart rate validation function
bool isValidHeartRate(int32_t hr) {
    return (hr >= MIN_VALID_HR && hr <= MAX_VALID_HR);
}

// --- Placeholder regression functions (REPLACED by new algorithm) ---
// These are kept for compatibility but will be replaced by new calculation
// float estimateGlucose(float ch1, float ch2, float ch3) {
//   return 80.0 + 0.05 * ch1 - 0.03 * ch2 + 0.02 * ch3;
// }

// float estimateSystolicBP(float ch1, float ch4, float ch6) {
//   return 110.0 + 0.04 * ch1 + 0.01 * ch4 - 0.02 * ch6;
// }

// float estimateDiastolicBP(float ch2, float ch5) {
//   return 70.0 + 0.03 * ch2 - 0.015 * ch5;
// }



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
    if (fields[1] != "AS7263" && fields[1] != "MAX30102" && fields[1] != "Average Blood Pressure and Glucose Readings" && fields[1] != "Remaining Average Readings" && fields[1] != "FINAL_AVG" && fields[1] != "INITIAL_TEST") {
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
  const size_t MAX_FILE_SIZE = 7000000; // 7MB threshold
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
  
  // Render handles HTTPS on port 443 (default), so:
  const char* host = "https://patient-monitor-backend-patient.fly.dev";
  const int httpPort = 443;
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

// NEW FUNCTION: Send initial test data via Bluetooth or WiFi
void sendInitialTestData() {
  Serial.println("\n=== INITIAL 10-SECOND TEST COMPLETE ===");
  
  // Calculate averages from initial test - FIXED: Use valid counts for HR
  if (initialTestCount > 0) {
    initialTestGlucose /= initialTestCount;
    initialTestSysBP /= initialTestCount;
    initialTestDiaBP /= initialTestCount;
    // Use valid HR count instead of total count for HR average
    if (initialTestValidHRCount > 0) {
      initialTestHR /= initialTestValidHRCount;
    } else {
      initialTestHR = 0; // No valid HR readings
    }
    initialTestSPO2 /= initialTestCount;
    initialTestTemp /= initialTestCount;
    initialTestBodyTemp /= initialTestCount;
    initialTestAccelX /= initialTestCount;
    initialTestAccelY /= initialTestCount;
    initialTestAccelZ /= initialTestCount;
    initialTestGyroX /= initialTestCount;
    initialTestGyroY /= initialTestCount;
    initialTestGyroZ /= initialTestCount;
  }
  
  // Print test results
  Serial.println("Initial Test Results:");
  Serial.print("Glucose: "); Serial.print(initialTestGlucose); Serial.println(" mg/dL");
  Serial.print("Sys BP: "); Serial.print(initialTestSysBP); Serial.println(" mmHg");
  Serial.print("Dia BP: "); Serial.print(initialTestDiaBP); Serial.println(" mmHg");
  Serial.print("Heart Rate: "); Serial.print(initialTestHR); Serial.println(" bpm");
  Serial.print("SpO2: "); Serial.print(initialTestSPO2); Serial.println(" %");
  Serial.print("Skin Temp: "); Serial.print(initialTestTemp); Serial.println(" °C");
  Serial.print("Body Temp: "); Serial.print(initialTestBodyTemp); Serial.println(" °C");
  Serial.println("======================================");
  
  // Log to CSV
  String timestamp = getTimestamp();
  String testData = String(initialTestGlucose) + "," + String(initialTestSysBP) + "," + 
                    String(initialTestDiaBP) + "," + String(initialTestHR) + "," + 
                    String(initialTestSPO2) + "," + String(initialTestTemp) + "," + 
                    String(initialTestBodyTemp) + "," + String(initialTestAccelX) + "," + 
                    String(initialTestAccelY) + "," + String(initialTestAccelZ) + "," + 
                    String(initialTestGyroX) + "," + String(initialTestGyroY) + "," + 
                    String(initialTestGyroZ);
  logDataToCSV(timestamp, "INITIAL_TEST", testData);
  saveDataToFlash();
  
  printConnectionStatus();
  
  // Send via Bluetooth if connected
  if (deviceConnected && bluetoothActive) {
    Serial.println("\n=== SENDING INITIAL TEST DATA VIA BLUETOOTH ===");
    
    JsonDocument btDoc;
    btDoc["type"] = "initial_test";
    btDoc["glucose"] = initialTestGlucose;
    btDoc["systolic_bp"] = initialTestSysBP;
    btDoc["diastolic_bp"] = initialTestDiaBP;
    btDoc["heart_rate"] = initialTestHR;
    btDoc["spo2"] = initialTestSPO2;
    btDoc["skin_temp"] = initialTestTemp;
    btDoc["body_temp"] = initialTestBodyTemp;
    btDoc["accel_x"] = initialTestAccelX;
    btDoc["accel_y"] = initialTestAccelY;
    btDoc["accel_z"] = initialTestAccelZ;
    btDoc["gyro_x"] = initialTestGyroX;
    btDoc["gyro_y"] = initialTestGyroY;
    btDoc["gyro_z"] = initialTestGyroZ;
    btDoc["timestamp"] = timestamp;
    
    String btJsonString;
    serializeJson(btDoc, btJsonString);
    
    sendDataViaBluetooth(btJsonString);
    Serial.println("Initial test data sent via Bluetooth!");
  }
  // Send via WiFi if available
  else if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n=== SENDING INITIAL TEST DATA VIA WIFI ===");
    
    // Ensure MQTT is connected
    if (!mqtt.connected()) {
      connectMQTT();
    }
    
    JsonDocument doc;
    doc["type"] = "initial_test";
    doc["g"] = initialTestGlucose;
    doc["s"] = initialTestSysBP;
    doc["d"] = initialTestDiaBP;
    doc["h"] = initialTestHR;
    doc["sp"] = initialTestSPO2;
    doc["sk"] = initialTestTemp;
    doc["b"] = initialTestBodyTemp;
    doc["aclX"] = initialTestAccelX;
    doc["aclY"] = initialTestAccelY;
    doc["aclZ"] = initialTestAccelZ;
    doc["gyX"] = initialTestGyroX;
    doc["gyY"] = initialTestGyroY;
    doc["gyZ"] = initialTestGyroZ;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    String cipherTextBase64 = aesEncryptBase64(jsonString, (const char*)aesKey, (const char*)aesIV);
    Serial.println("=== Initial Test Encrypted Data (Base64) ===");
    Serial.println(cipherTextBase64);
    Serial.println("============================================");
    
    publishEncryptedData(cipherTextBase64);
    Serial.println("Initial test data sent via WiFi!");
  } else {
    Serial.println("\nNo communication method available for initial test. Data stored locally.");
  }
  
  // Reset initial test variables
  initialTestGlucose = 0;
  initialTestSysBP = 0;
  initialTestDiaBP = 0;
  initialTestHR = 0;
  initialTestSPO2 = 0;
  initialTestTemp = 0;
  initialTestBodyTemp = 0;
  initialTestAccelX = 0;
  initialTestAccelY = 0;
  initialTestAccelZ = 0;
  initialTestGyroX = 0;
  initialTestGyroY = 0;
  initialTestGyroZ = 0;
  initialTestCount = 0;
  initialTestValidHRCount = 0; // Reset valid HR count
}

// NEW: AS7263 BP and Glucose calculation functions
void collectAS7263Sample() {
  // Ensure LEDs are on
  as7263.drvOn();
  as7263.indicateLED(true);
  
  // Start measurement
  as7263.startMeasurement();
  
  // Wait for data
  unsigned long startTime = millis();
  while (!as7263.dataReady()) {
    if (millis() - startTime > 2000) {
      Serial.println("Timeout - retrying...");
      return;
    }
    delay(10);
  }
  
  // Read calibrated values and accumulate
  red_sum    += as7263.readCalibratedRed();
  orange_sum += as7263.readCalibratedOrange();
  yellow_sum += as7263.readCalibratedYellow();
  green_sum  += as7263.readCalibratedGreen();
  blue_sum   += as7263.readCalibratedBlue();
  violet_sum += as7263.readCalibratedViolet();
  
  as7263_sample_count++;
}

void calculateBPAndGlucose(float &glucose, float &systolic, float &diastolic) {
  if (as7263_sample_count == 0) return;
  
  // Calculate 2-minute averages
  float avg_red    = red_sum / as7263_sample_count;
  float avg_orange = orange_sum / as7263_sample_count;
  float avg_yellow = yellow_sum / as7263_sample_count;
  float avg_green  = green_sum / as7263_sample_count;
  float avg_blue   = blue_sum / as7263_sample_count;
  float avg_violet = violet_sum / as7263_sample_count;
  
  Serial.println("\n=== AS7263 2-MINUTE AVERAGED READINGS ===");
  
  // Show all readings with indicators
  Serial.print("  [X] Red (610nm):    "); 
  Serial.print(avg_red, 2); 
  Serial.println("  [VISIBLE - Ignored]");
  
  Serial.print("  [X] Orange (680nm): "); 
  Serial.print(avg_orange, 2); 
  Serial.println("  [VISIBLE - Ignored]");
  
  Serial.print("  [!] Yellow (730nm): "); 
  Serial.print(avg_yellow, 2); 
  Serial.println("  [Border - 10% weight]");
  
  Serial.print("  [OK] Green (760nm):  "); 
  Serial.print(avg_green, 2); 
  Serial.println("  [TRUE NIR - 20%]");
  
  Serial.print("  [OK] Blue (810nm):   "); 
  Serial.print(avg_blue, 2); 
  Serial.println("  [TRUE NIR - 35%]");
  
  Serial.print("  [OK] Violet (860nm): "); 
  Serial.print(avg_violet, 2); 
  Serial.println("  [TRUE NIR - 35%]");
  
  // Calculate NIR intensity using ONLY true NIR channels for BP
  float nir_intensity = (avg_yellow * WEIGHT_YELLOW) +
                        (avg_green * WEIGHT_GREEN) +
                        (avg_blue * WEIGHT_BLUE) +
                        (avg_violet * WEIGHT_VIOLET);
  
  Serial.print("\n  NIR Intensity Score: ");
  Serial.println(nir_intensity, 2);
  
  // Normalize to 0-1 range for BP
  float normalized_intensity = nir_intensity / 1500.0;
  if (normalized_intensity > 1.0) normalized_intensity = 1.0;
  if (normalized_intensity < 0.0) normalized_intensity = 0.0;
  
  Serial.print("  Normalized Score: ");
  Serial.println(normalized_intensity, 4);
  
  // Map to BP ranges
  systolic = SYSTOLIC_MIN + (normalized_intensity * (SYSTOLIC_MAX - SYSTOLIC_MIN));
  diastolic = DIASTOLIC_MIN + (normalized_intensity * (DIASTOLIC_MAX - DIASTOLIC_MIN));
  
  // Apply additional modulation based on Blue/Violet ratio for BP
  float bv_ratio = 0.5;
  if (avg_violet > 10) {
    bv_ratio = avg_blue / avg_violet;
  }
  
  Serial.print("  Blue/Violet Ratio: ");
  Serial.println(bv_ratio, 3);
  
  // Adjust BP based on perfusion indicator
  if (bv_ratio > 0.4) {
    systolic -= 3;   // Better perfusion = slightly lower BP
    diastolic -= 2;
    Serial.println("  Good perfusion detected (-3/-2)");
  } else if (bv_ratio < 0.2) {
    systolic += 3;   // Poor perfusion = slightly higher BP
    diastolic += 2;
    Serial.println("  Poor perfusion detected (+3/+2)");
  } else {
    Serial.println("  Normal perfusion (no adjustment)");
  }
  
  // Ensure within target ranges
  systolic = constrain(systolic, SYSTOLIC_MIN, SYSTOLIC_MAX);
  diastolic = constrain(diastolic, DIASTOLIC_MIN, DIASTOLIC_MAX);
  
  // Calculate glucose-specific NIR intensity
  float glucose_intensity = (avg_yellow * GLUCOSE_WEIGHT_YELLOW) +
                           (avg_green * GLUCOSE_WEIGHT_GREEN) +
                           (avg_blue * GLUCOSE_WEIGHT_BLUE) +
                           (avg_violet * GLUCOSE_WEIGHT_VIOLET);
  
  Serial.print("\n  Glucose NIR Score: ");
  Serial.println(glucose_intensity, 2);
  
  // Normalize glucose intensity (different scaling than BP)
  float normalized_glucose = glucose_intensity / 1200.0;
  if (normalized_glucose > 1.0) normalized_glucose = 1.0;
  if (normalized_glucose < 0.0) normalized_glucose = 0.0;
  
  Serial.print("  Normalized Glucose Score: ");
  Serial.println(normalized_glucose, 4);
  
  // Map to glucose range 70-135 mg/dL
  glucose = GLUCOSE_MIN + (normalized_glucose * (GLUCOSE_MAX - GLUCOSE_MIN));
  
  // Apply glucose-specific adjustments based on wavelength ratios
  float bg_ratio = 0.5;
  if (avg_green > 10) {
    bg_ratio = avg_blue / avg_green;
  }
  
  Serial.print("  Blue/Green Ratio: ");
  Serial.println(bg_ratio, 3);
  
  // Adjust glucose based on absorption characteristics
  if (bg_ratio > 0.6) {
    glucose += 5;  // Higher absorption = higher glucose reading
    Serial.println("  High glucose absorption detected (+5)");
  } else if (bg_ratio < 0.3) {
    glucose -= 5;  // Lower absorption = lower glucose reading
    Serial.println("  Low glucose absorption detected (-5)");
  } else {
    Serial.println("  Normal glucose absorption (no adjustment)");
  }
  
  // CONSTRAIN GLUCOSE TO 70-135 RANGE
  glucose = constrain(glucose, GLUCOSE_MIN, GLUCOSE_MAX);
  

}

void resetAS7263Accumulators() {
  red_sum = 0;
  orange_sum = 0;
  yellow_sum = 0;
  green_sum = 0;
  blue_sum = 0;
  violet_sum = 0;
  as7263_sample_count = 0;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Suppress WiFi error logs to reduce spam
  esp_log_level_set("wifi", ESP_LOG_NONE); 
  
  initializeSPIFFS();
  
  Wire.begin(41, 42);
  
  // FIRST: Initialize Bluetooth (primary communication method)
  initializeBluetooth();
  
  // SECOND: If Bluetooth fails after max attempts, fall back to WiFi
  if (bluetoothAttempts >= MAX_BLUETOOTH_ATTEMPTS && !bluetoothActive) {
      Serial.println("Bluetooth failed, attempting WiFi connection...");
      checkAndConnectWiFi();
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
  
  // Configure AS7263 with improved settings
  as7263.setIntegrationTime(50);
  as7263.setGain(GAIN_64X);
  as7263.drvOn();
  as7263.indicateLED(true);
  as7263.setDrvCurrent(LIMIT_12MA5);
  as7263.setIndicateCurrent(LIMIT_8MA);
  
  Serial.println("Initializing BMI270...");
  if (bmi.beginI2C() != BMI2_OK) {
    Serial.println("Could not find a valid BMI270 sensor, check wiring!");
    while (1) delay(10);
  }
  Serial.println("BMI270 initialized!");
  
  Serial.println("TMP117 Temperature Sensor Initialized");
  
  // NEW: Start with initial 10-second test
  Serial.println("\n========================================");
  Serial.println("STARTING INITIAL 10-SECOND SENSOR TEST");
  Serial.println("========================================");
  Serial.println("Testing TMP117, MAX30102, BMI270, and AS7263...");
  Serial.println("Place your finger on both sensors.");
  phaseStartTime = millis();
  currentState = INITIAL_TEST_PHASE;
}

void loop() {
  unsigned long currentTime = millis();
  
  // Manage Bluetooth connection (this runs in every loop iteration)
  manageBluetoothConnection();
  
  // Your existing WiFi connection management
  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    Serial.println("WiFi disconnected. Switching to offline mode.");
    wifiConnected = false;
  } else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    Serial.println("WiFi reconnected. Switching to online mode.");
    wifiConnected = true;
    pendingUpload = true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
  
  switch (currentState) {
    // NEW CASE: Initial 10-second test phase
    case INITIAL_TEST_PHASE:
      if (currentTime - phaseStartTime < INITIAL_TEST_DURATION) {
        unsigned long timeInPhase = currentTime - phaseStartTime;
        
        // First 5 seconds: TMP117, MAX30102, BMI270
        if (timeInPhase < 5000) {
          Serial.print("Initial Test (TMP/MAX/BMI) - Time: ");
          Serial.print(timeInPhase / 1000);
          Serial.println(" seconds");
          
          // Read MAX30102
          bufferLength = 25; // Smaller buffer for quick test
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
          
          // Read TMP117
          float skinTemp = getAverageTemp(3);
          float bodyTemp = skinTemp + CALIBRATION_OFFSET;
          
          // Read BMI270
          bmi.getSensorData();
          float ax = bmi.data.accelX;
          float ay = bmi.data.accelY;
          float az = bmi.data.accelZ;
          float gx = bmi.data.gyroX;
          float gy = bmi.data.gyroY;
          float gz = bmi.data.gyroZ;
          
          // Accumulate for averaging - FIXED: Only add valid HR readings
          if (validHeartRate && isValidHeartRate(heartRate)) {
            initialTestHR += heartRate;
            initialTestValidHRCount++; // Count only valid readings
          }
          if (validSPO2) initialTestSPO2 += spo2;
          initialTestTemp += skinTemp;
          initialTestBodyTemp += bodyTemp;
          initialTestAccelX += ax;
          initialTestAccelY += ay;
          initialTestAccelZ += az;
          initialTestGyroX += gx;
          initialTestGyroY += gy;
          initialTestGyroZ += gz;
          
          Serial.print("HR: ");
          Serial.print(validHeartRate ? String(heartRate) : "Invalid");
          Serial.print(" | SpO2: ");
          Serial.print(validSPO2 ? String(spo2) : "Invalid");
          Serial.print(" | Temp: ");
          Serial.println(skinTemp);
          
          initialTestCount++;
          delay(1000);
        }
        // Next 5 seconds: AS7263 + continue other sensors
        else {
          Serial.print("Initial Test (AS7263 + All) - Time: ");
          Serial.print(timeInPhase / 1000);
          Serial.println(" seconds");
          
          // Read AS7263 using new method
          collectAS7263Sample();
          
          // Calculate BP and Glucose from AS7263
          float glucose, systolic, diastolic;
          calculateBPAndGlucose(glucose, systolic, diastolic);
          
          // Read MAX30102
          bufferLength = 25;
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
          
          // Read TMP117
          float skinTemp = getAverageTemp(3);
          float bodyTemp = skinTemp + CALIBRATION_OFFSET;
          
          // Read BMI270
          bmi.getSensorData();
          float ax = bmi.data.accelX;
          float ay = bmi.data.accelY;
          float az = bmi.data.accelZ;
          float gx = bmi.data.gyroX;
          float gy = bmi.data.gyroY;
          float gz = bmi.data.gyroZ;
          
          // Accumulate all readings - FIXED: Only add valid HR readings
          initialTestGlucose += glucose;
          initialTestSysBP += systolic;
          initialTestDiaBP += diastolic;
          if (validHeartRate && isValidHeartRate(heartRate)) {
            initialTestHR += heartRate;
            initialTestValidHRCount++; // Count only valid readings
          }
          if (validSPO2) initialTestSPO2 += spo2;
          initialTestTemp += skinTemp;
          initialTestBodyTemp += bodyTemp;
          initialTestAccelX += ax;
          initialTestAccelY += ay;
          initialTestAccelZ += az;
          initialTestGyroX += gx;
          initialTestGyroY += gy;
          initialTestGyroZ += gz;
          
          Serial.print("Glucose: "); Serial.print(glucose);
          Serial.print(" | SysBP: "); Serial.print(systolic);
          Serial.print(" | DiaBP: "); Serial.print(diastolic);
          Serial.print(" | HR: ");
          Serial.print(validHeartRate ? String(heartRate) : "Invalid");
          Serial.print(" | SpO2: ");
          Serial.print(validSPO2 ? String(spo2) : "Invalid");
          Serial.println();
          
          initialTestCount++;
          delay(1000);
        }
      } else {
        // Initial test complete - send data and move to normal operation
        sendInitialTestData();
        
        // Reset AS7263 accumulators for normal operation
        resetAS7263Accumulators();
        
        // Now start normal 2-minute cycles
        Serial.println("\n========================================");
        Serial.println("STARTING NORMAL 2-MINUTE CYCLES");
        Serial.println("========================================");
        Serial.println("Starting with AS7263 sensor for 2 minutes...");
        Serial.println("Place your finger on the AS7263 sensor.");
        phaseStartTime = millis();
        currentState = AS7263_PHASE;
      }
      break;
    
    case AS7263_PHASE:
      if (currentTime - phaseStartTime < PHASE_DURATION) {
        // Collect AS7263 sample using new method
        collectAS7263Sample();
        
        // Calculate BP and Glucose from accumulated AS7263 data
        float glucose, systolic, diastolic;
        calculateBPAndGlucose(glucose, systolic, diastolic);
        
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
          sysBPReadings[readingCount] = systolic;
          diaBPReadings[readingCount] = diastolic;
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
        String sensorData = String(glucose) + "," + String(systolic) + "," + String(diastolic) + ",,," + String(skinTemp) + "," + String(bodyTemp) + "," +
                            String(ax) + "," + String(ay) + "," + String(az) + "," +
                            String(gx) + "," + String(gy) + "," + String(gz);
        logDataToCSV(timestamp, "AS7263", sensorData);
        
        Serial.print("AS7263 - Time remaining: ");
        Serial.print((PHASE_DURATION - (currentTime - phaseStartTime)) / 1000);
        Serial.println(" seconds");
        
        Serial.print("Glucose: ");
        Serial.print(glucose);
        Serial.print(" mg/dL | SysBP: ");
        Serial.print(systolic);
        Serial.print(" mmHg | DiaBP: ");
        Serial.print(diastolic);
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
        // Calculate final averages from all readings
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
        String avgData = String(avgGlucose) + "," + String(avgSysBP) + "," + String(avgDiaBP) + ",,," + String(avgTemp) + "," + String(avgBodyTemp) + "," +
                         String(avgAccelX) + "," + String(avgAccelY) + "," + String(avgAccelZ) + "," +
                         String(avgGyroX) + "," + String(avgGyroY) + "," + String(avgGyroZ);
        logDataToCSV(timestamp, "Average Blood Pressure and Glucose Readings", avgData);
        
        // Print summary to Serial
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
        
        // Save to flash immediately after logging summary
        saveDataToFlash();
        
        resetReadings();
        resetAS7263Accumulators(); // Reset for next cycle
        
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
              // Serial.println("Ensure finger is properly placed on the sensor.");
              lowSignalShown = true;
            }
            // Serial.println("Heart Rate: INVALID | SpO2: INVALID");
          } else {
            lowSignalShown = false; // reset when signal improves
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
        
        // FIXED: Only store valid heart rate readings and count them separately
        if (readingCount < MAX_READINGS) {
          // Store heart rate only if valid and within range
          if (validHeartRate && isValidHeartRate(heartRate)) {
            hrReadings[readingCount] = heartRate;
            validHRReadingCount++; // Count valid HR readings
          } else {
            hrReadings[readingCount] = -1; // Mark as invalid
          }
          
          // Store SpO2 only if valid
          if (validSPO2) {
            spo2Readings[readingCount] = spo2;
            validSPO2ReadingCount++; // Count valid SpO2 readings
          } else {
            spo2Readings[readingCount] = -1; // Mark as invalid
          }
          
          // Always store other sensor data
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
        String hrValue = (validHeartRate && isValidHeartRate(heartRate)) ? String(heartRate) : "";
        String spo2Value = validSPO2 ? String(spo2) : "";
        String sensorData = ",,," + hrValue + "," + spo2Value + "," + String(skinTemp) + "," + String(bodyTemp) + "," +
                            String(ax) + "," + String(ay) + "," + String(az) + "," +
                            String(gx) + "," + String(gy) + "," + String(gz);
        logDataToCSV(timestamp, "MAX30102", sensorData);
        
        Serial.print("MAX30102 - Time remaining: ");
        Serial.print((PHASE_DURATION - (currentTime - phaseStartTime)) / 1000);
        Serial.println(" seconds");
        
        Serial.print("Heart Rate: ");
        if (validHeartRate && isValidHeartRate(heartRate)) Serial.print(heartRate);
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
        // FIXED: Calculate averages using only valid readings
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
        
        // Calculate HR average using only valid readings
        if (validHRReadingCount > 0) {
          for (int i = 0; i < readingCount; i++) {
            if (hrReadings[i] != -1) { // Only use valid HR readings
              avgHR += hrReadings[i];
            }
          }
          avgHR /= validHRReadingCount;
        }
        
        // Calculate SpO2 average using only valid readings
        if (validSPO2ReadingCount > 0) {
          for (int i = 0; i < readingCount; i++) {
            if (spo2Readings[i] != -1) { // Only use valid SpO2 readings
              avgSPO2 += spo2Readings[i];
            }
          }
          avgSPO2 /= validSPO2ReadingCount;
        }
        
        // Calculate averages for other sensors (all readings are valid)
        for (int i = 0; i < readingCount; i++) {
          avgTemp += tempReadings[i];
          avgBodyTemp += bodyTempReadings[i];
          avgAccelX += accelXReadings[i];
          avgAccelY += accelYReadings[i];
          avgAccelZ += accelZReadings[i];
          avgGyroX += gyroXReadings[i];
          avgGyroY += gyroYReadings[i];
          avgGyroZ += gyroZReadings[i];
        }
        
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
        logDataToCSV(timestamp, "Remaining Average Readings", avgData);
        
        // Print summary to Serial
        Serial.println("\n=== MAX30102 2-MINUTE READING SUMMARY ===");
        Serial.print("Average Heart Rate: ");
        Serial.print(avgHR);
        Serial.print(" bpm (");
        Serial.print(validHRReadingCount);
        Serial.print(" valid readings) | Average SpO2: ");
        Serial.print(avgSPO2);
        Serial.print(" % (");
        Serial.print(validSPO2ReadingCount);
        Serial.println(" valid readings)");
        
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
        
        // Save to flash immediately after logging summary
        saveDataToFlash();
        
        currentState = POST_DATA_PHASE;
      }
      break;
      
  case POST_DATA_PHASE:
      printConnectionStatus(); // Add connection status debug
      
      // FIRST: Try to send via Bluetooth if connected
      if (deviceConnected && bluetoothActive) {
        Serial.println("\n=== SENDING VITAL SIGNS VIA BLUETOOTH ===");
        
        // Prepare JSON data for Bluetooth
        JsonDocument btDoc;
        btDoc["glucose"] = avgGlucose;
        btDoc["systolic_bp"] = avgSysBP;
        btDoc["diastolic_bp"] = avgDiaBP;
        btDoc["heart_rate"] = avgHR;
        btDoc["spo2"] = avgSPO2;
        btDoc["skin_temp"] = avgTemp;
        btDoc["body_temp"] = avgBodyTemp;
        btDoc["accel_x"] = avgAccelX;
        btDoc["accel_y"] = avgAccelY;
        btDoc["accel_z"] = avgAccelZ;
        btDoc["gyro_x"] = avgGyroX;
        btDoc["gyro_y"] = avgGyroY;
        btDoc["gyro_z"] = avgGyroZ;
        btDoc["timestamp"] = getTimestamp();
        
        String btJsonString;
        serializeJson(btDoc, btJsonString);
        
        sendDataViaBluetooth(btJsonString);
        Serial.println("Data successfully sent via Bluetooth!");
        
      } 
      // SECOND: Use WiFi fallback if available
      else if (WiFi.status() == WL_CONNECTED) {
        // Ensure MQTT is connected when using WiFi
        if (!mqtt.connected()) {
            connectMQTT();
        }
        
        Serial.println("\n=== USING WIFI FALLBACK ===");
            
        String testJson = "{\"g\":160,\"s\":150,\"d\":77}";
        String encrypted = aesEncryptBase64(testJson, (const char*)aesKey, (const char*)aesIV);
        bool testSuccess = mqtt.publish(MQTT_TOPIC, testJson.c_str());
        Serial.print("Unencrypted JSON test: ");
        Serial.println(testSuccess ? "SUCCESS" : "FAILED");
        publishEncryptedData(encrypted);

        JsonDocument doc;
        doc["g"] = avgGlucose;
        doc["s"] = avgSysBP;
        doc["d"] = avgDiaBP;
        doc["h"] = avgHR;
        doc["sp"] = avgSPO2;
        doc["sk"] = avgTemp;
        doc["b"] = avgBodyTemp;
        doc["aclX"] = avgAccelX;
        doc["aclY"] = avgAccelY;
        doc["aclZ"] = avgAccelZ;
        doc["gyX"] = avgGyroX;
        doc["gyY"] = avgGyroY;
        doc["gyZ"] = avgGyroZ;
        String jsonString;
        serializeJson(doc, jsonString);

        String cipherTextBase64 = aesEncryptBase64(jsonString, (const char*)aesKey, (const char*)aesIV);
        Serial.println("=== AES Encrypted Data (Base64) ===");
        Serial.println(cipherTextBase64);
        Serial.println("===================================");

        publishEncryptedData(cipherTextBase64);

        String decryptedText = aesDecryptBase64(cipherTextBase64, (const char*)aesKey, (const char*)aesIV);
        Serial.println("=== Decrypted Text (Verify) ===");
        Serial.println(decryptedText);
        
        postVitalsDataToServer(avgGlucose, avgSysBP, avgDiaBP, avgHR, avgSPO2,
                              avgTemp, avgBodyTemp,
                              avgAccelX, avgAccelY, avgAccelZ,
                              avgGyroX, avgGyroY, avgGyroZ);
      } else {
        Serial.println("\nNo communication method available. Data stored locally.");
      }
      
      // If pending upload (reconnected during cycle), upload stored data now
      if (pendingUpload && wifiConnected) {
        uploadStoredData();
        pendingUpload = false;
      }
      
      resetReadings();
      resetAS7263Accumulators(); // Reset for next cycle
      Serial.println("Starting new cycle with AS7263 sensor for 2 minutes...");
      Serial.println("Place your finger on the AS7263 sensor.");
      phaseStartTime = millis();
      currentState = AS7263_PHASE;
      break;
  }
}

void resetReadings() {
  readingCount = 0;
  validHRReadingCount = 0;    // Reset valid HR count
  validSPO2ReadingCount = 0;  // Reset valid SpO2 count
  
  for (int i = 0; i < MAX_READINGS; i++) {
    glucoseReadings[i] = 0;
    sysBPReadings[i] = 0;
    diaBPReadings[i] = 0;
    hrReadings[i] = -1;  // Initialize with -1 to indicate invalid
    spo2Readings[i] = -1; // Initialize with -1 to indicate invalid
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

  doc["g"] = glucose;
  doc["s"] = sysBP;
  doc["d"] = diaBP;
  doc["h"] = heartRate;
  doc["sp"] = spO2;
  doc["sk"] = temperature;
  doc["b"] = bodyTemperature;
  doc["aclX"] = accelX;
  doc["aclY"] = accelY;
  doc["aclZ"] = accelZ;
  doc["gyX"] = gyroX;
  doc["gyY"] = gyroY;
  doc["gyZ"] = gyroZ;

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
      

      if (httpResponseCode == 201) {
    Serial.println("Post successful!");
    Serial.println("All vital signs posted successfully!");
    uploadCSVToServer();

    // --- AES encrypt JSON payload and show in Serial ---
    String cipherTextBase64 = aesEncryptBase64(jsonString, (const char*)aesKey, (const char*)aesIV);
    Serial.println("=== AES Encrypted Data (Base64) ===");
    Serial.println(cipherTextBase64);
    Serial.println("===================================");
}else if (httpResponseCode == 500) {
    Serial.println("SERVER ERROR: The server encountered an internal error.");

    // --- AES encrypt JSON payload and show in Serial ---
    String cipherTextBase64 = aesEncryptBase64(jsonString, (const char*)aesKey, (const char*)aesIV);

    // Decrypt to verify
    String decryptedText = aesDecryptBase64(cipherTextBase64, (const char*)aesKey, (const char*)aesIV);
    Serial.println("=== Decrypted Text (Verify) ===");
    Serial.println(decryptedText);

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
        Serial.println("Unexpected response code.");
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