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
