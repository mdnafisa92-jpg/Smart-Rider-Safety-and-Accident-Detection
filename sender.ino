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
