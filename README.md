# Smart-Rider-Safety-and-Accident-Detection
# Helmet module code
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <MPU6050.h>
#include <esp_wifi.h>

#define HELMET_PIN 26
#define EYE_PIN    27
#define MQ3_PIN    34
#define SDA_PIN    21
#define SCL_PIN    22

#define ALCOHOL_THRESHOLD   800
#define ACC_THRESHOLD       2.5
#define EYE_CLOSE_TIME_MS   2000

uint8_t bikeMAC[] = {0xBC, 0xDD, 0xC2, 0xD1, 0xD6, 0xF4};

typedef struct {
  uint8_t helmet;
  uint8_t alcohol;
  uint8_t eyeClosed;
  uint8_t accident;
} helmet_data_t;

helmet_data_t data;
MPU6050 mpu;
unsigned long eyeCloseStartTime = 0;

void onSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);

  pinMode(HELMET_PIN, INPUT_PULLUP);
  pinMode(EYE_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  mpu.initialize();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Force fixed channel
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bikeMAC, 6);
  peer.channel = 1;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  Serial.println("Sender ready (channel 1)");
}

void loop() {

  data.helmet = (digitalRead(HELMET_PIN) == HIGH);

  int mq3Value = analogRead(MQ3_PIN);
  data.alcohol = (mq3Value > ALCOHOL_THRESHOLD);

  if (digitalRead(EYE_PIN) == LOW) {
    if (eyeCloseStartTime == 0)
      eyeCloseStartTime = millis();

    if (millis() - eyeCloseStartTime >= EYE_CLOSE_TIME_MS)
      data.eyeClosed = 1;
  } else {
    eyeCloseStartTime = 0;
    data.eyeClosed = 0;
  }

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float Ax = ax / 16384.0;
  float Ay = ay / 16384.0;
  float Az = az / 16384.0;
  float accMag = sqrt(Ax * Ax + Ay * Ay + Az * Az);

  data.accident = (accMag > ACC_THRESHOLD);

  esp_now_send(bikeMAC, (uint8_t*)&data, sizeof(data));

  delay(500);
}




# Bike module code
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TinyGPSPlus.h>
#include <HTTPClient.h>
#include "base64.h"

#define RELAY_PIN   25
#define BUZZER_PIN  26
#define GPS_RX      16
#define GPS_TX      17

#define ACC_CONFIRM_TIME 800
#define HELMET_DEBOUNCE_TIME 300

/* ================= WIFI FOR TWILIO ================= */
const char* ssid = "lohi";
const char* password = "pranavi24";

/* ================= TWILIO CONFIG ================= */
const char* account_sid = "AC876b419b0c121adb0f3e39780d8e9f17";
const char* auth_token  = "4074950f50b3002ca53d4992ede7e7b9";

String fromNumber = "+19893680062";

/* ===== MULTIPLE RECEIVER NUMBERS ===== */
String toNumbers[] = {
  "+917675856779",
  "+919542943919",
  "+917093929388"
};

const int totalNumbers = sizeof(toNumbers) / sizeof(toNumbers[0]);

/* ================= GPS ================= */
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

/* ================= DATA STRUCTURE ================= */
typedef struct struct_message {
  bool helmetWorn;
  bool alcohol;
  bool sleep;
  bool accident;
} struct_message;

struct_message incomingData;

/* ================= STATE VARIABLES ================= */
bool accidentConfirmed = false;
bool accidentTimerRunning = false;
bool locationSent = false;
unsigned long accidentStartTime = 0;

bool stableHelmetWorn = false;
bool lastHelmetRead = false;
unsigned long helmetChangeTime = 0;

/* ================= SMS FUNCTION ================= */
void sendSMS(double lat, double lon) {

  Serial.println("Connecting to WiFi for SMS...");
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    for (int i = 0; i < totalNumbers; i++) {

      HTTPClient http;

      String url = "https://api.twilio.com/2010-04-01/Accounts/" +
                   String(account_sid) + "/Messages.json";

      http.begin(url);

      String auth = String(account_sid) + ":" + String(auth_token);
      String encodedAuth = base64::encode(auth);

      http.addHeader("Authorization", "Basic " + encodedAuth);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");

      String mapsLink = "https://www.google.com/maps?q=" +
                        String(lat, 6) + "," +
                        String(lon, 6);

      String messageBody =
         "Accident+Detected%21"
         "%0ALocation%3A+" + mapsLink;

      String postData =
        "To=" + toNumbers[i] +
        "&From=" + fromNumber +
        "&Body=" + messageBody;

      int code = http.POST(postData);

      Serial.print("SMS to ");
      Serial.print(toNumbers[i]);
      Serial.print(" | HTTP code: ");
      Serial.println(code);

      http.end();
      delay(3000); // delay between messages
    }

  } else {
    Serial.println("WiFi failed. SMS not sent.");
  }

  /* Return to ESP-NOW mode */
  WiFi.disconnect(true);
  delay(100);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.println("Returned to ESP-NOW mode");
}

/* ================= RECEIVE CALLBACK ================= */
void onDataRecv(const uint8_t *mac,
                const uint8_t *incomingDataRaw,
                int len) {

  if (len != sizeof(incomingData)) return;
  memcpy(&incomingData, incomingDataRaw, sizeof(incomingData));

  Serial.println("\n====== RECEIVED DATA ======");
  Serial.print("Helmet worn     : ");
  Serial.println(incomingData.helmetWorn ? "YES" : "NO");

  Serial.print("Alcohol detected: ");
  Serial.println(incomingData.alcohol ? "YES" : "NO");

  Serial.print("Eye closed long : ");
  Serial.println(incomingData.sleep ? "YES" : "NO");

  Serial.print("Accident signal : ");
  Serial.println(incomingData.accident ? "YES" : "NO");

  /* Helmet debounce */
  bool currentHelmetRead = incomingData.helmetWorn;
  if (currentHelmetRead != lastHelmetRead) {
    helmetChangeTime = millis();
    lastHelmetRead = currentHelmetRead;
  }
  if (millis() - helmetChangeTime >= HELMET_DEBOUNCE_TIME) {
    stableHelmetWorn = currentHelmetRead;
  }

  /* Accident confirmation */
  if (incomingData.accident && !accidentConfirmed) {
    if (!accidentTimerRunning) {
      accidentStartTime = millis();
      accidentTimerRunning = true;
    }

    if (millis() - accidentStartTime >= ACC_CONFIRM_TIME) {
      accidentConfirmed = true;
      accidentTimerRunning = false;
      locationSent = false;
      Serial.println("Accident confirmed");
    }
  }

  /* Reset after accident */
  if (!incomingData.accident && accidentConfirmed) {
    accidentConfirmed = false;
    accidentTimerRunning = false;
    accidentStartTime = 0;
    locationSent = false;
    Serial.println("System reset to normal");
  }

  /* ================= ENGINE CONTROL ================= */
  if (accidentConfirmed) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Engine: OFF (Accident)");
  }
  else if (!stableHelmetWorn) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Engine: OFF (Helmet not worn)");
  }
  else if (incomingData.alcohol) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Engine: OFF (Alcohol detected)");
  }
  else {
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Engine: ON");
  }

  /* ================= BUZZER ================= */
  if (incomingData.sleep) {
    digitalWrite(BUZZER_PIN, HIGH);
    Serial.println("Buzzer: ON (Drowsiness)");
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("Buzzer: OFF");
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("Receiver ready (channel 1)");
}

/* ================= LOOP ================= */
void loop() {

  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  /* Send SMS once after accident */
  if (accidentConfirmed && !locationSent && gps.location.isValid()) {

    double lat = gps.location.lat();
    double lon = gps.location.lng();

    Serial.println("\n=== ACCIDENT LOCATION ===");
    Serial.print("Latitude : ");
    Serial.println(lat, 6);
    Serial.print("Longitude: ");
    Serial.println(lon, 6);

    sendSMS(lat, lon);
    locationSent = true;
  }
}
