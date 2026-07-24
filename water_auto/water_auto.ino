#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <user_interface.h>
#include <include/WiFiState.h>
#include <EEPROM.h>

#define STA_RETRY 32

#define AP_SSID "MOTSYS_AP"
#define AP_PASS "MOTSYS_PASS"

#define LED_R 12
#define LED_G 13
#define BUZ 14
#define RELAY 5
#define DIN 4

//#define DEBUG 1

typedef struct ee_data {
  unsigned char ssid_len;
  char ssid[31];
  unsigned char pass_len;
  char pass[31];
  unsigned char hr_len;
  char hr[31];
} ee_data;

ee_data ee;

const char *PARAM_INPUT_1 = "input1";
const char *PARAM_INPUT_2 = "input2";
const char *PARAM_INPUT_3 = "input3";

const char s1[] PROGMEM = { "<!DOCTYPE HTML><html><head><title>PWRSYS Configuration</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body><form action=\"/get\">______WiFi_SSID:<input type=\"text\" name=\"input1\" value=\"" };
const char s2[] PROGMEM = { "\"><br><br>WiFi_PASSWORD:<input type=\"text\" name=\"input2\" value=\"" };
const char s3[] PROGMEM = { "\"><br><br>___________HOUR:<input type=\"text\" name=\"input3\" value=\"" };
const char s4[] PROGMEM = { "\"><input type=\"submit\" value=\"Reboot\"></form></body></html>" };
String index_html;

String ssid;
String pass;
String hr;
bool motor = false;
bool full = false;

void (*reset)(void) = 0;

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Invalid Request");
}

void setup() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(BUZ, OUTPUT);
  pinMode(RELAY, OUTPUT);

  digitalWrite(LED_R, HIGH);
#ifdef DEBUG
  Serial.begin(115200);
#endif
  EEPROM.begin(sizeof(ee_data));
  EEPROM.get(0, ee);
  ssid = String(ee.ssid);
  pass = String(ee.pass);
  hr = String(ee.hr);
#ifdef DEBUG
  Serial.printf("\nconfig: %s %s %s\n", ssid.c_str(), pass.c_str(), hr.c_str());
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
#ifdef DEBUG
    Serial.printf("Connecting to AP %s %s %d %d\n", ssid.c_str(), pass.c_str(), WiFi.status(), retry);
#endif
    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, LOW);
    delay(250);
    digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, HIGH);
    delay(250);
    retry++;
    if (retry >= STA_RETRY) break;
  }
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);

  if (retry >= STA_RETRY) {
    IPAddress apip(10, 10, 10, 1);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apip, apip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASS, 7, false, 1);

    index_html = String(s1) + ssid + String(s2) + pass + String(s3) + hr + String(s4);
    AsyncWebServer server(80);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", index_html.c_str());
    });
    server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
      if (request->hasParam(PARAM_INPUT_1)) {
        ssid = request->getParam(PARAM_INPUT_1)->value();
      }
      if (request->hasParam(PARAM_INPUT_2)) {
        pass = request->getParam(PARAM_INPUT_2)->value();
      }
      if (request->hasParam(PARAM_INPUT_3)) {
        hr = request->getParam(PARAM_INPUT_3)->value();
      }
#ifdef DEBUG
      Serial.printf("Web message %s %s %s\n", ssid.c_str(), pass.c_str(), hr.c_str());
#endif
      String ee_ssid;
      String ee_pass;
      String ee_hr;

      while ((ee_ssid != ssid) && (ee_pass != pass) && (ee_hr != hr)) {
        strcpy(ee.ssid, ssid.c_str());
        ee.ssid_len = ssid.length();
        strcpy(ee.pass, pass.c_str());
        ee.pass_len = pass.length();
        strcpy(ee.hr, hr.c_str());
        ee.hr_len = hr.length();
        EEPROM.put(0, ee);
        EEPROM.commit();
        delay(100);
        EEPROM.get(0, ee);
        ee_ssid = String(ee.ssid);
        ee_pass = String(ee.pass);
        ee_hr = String(ee.hr);
      }

      request->redirect("http://10.10.10.1");
      reset();
    });
    server.onNotFound(notFound);
    server.begin();

    while (1) {
      digitalWrite(BUILTIN_LED, HIGH);
      delay(500);
      digitalWrite(BUILTIN_LED, LOW);
      delay(500);
#ifdef DEBUG
      Serial.printf("AP mode webserver waiting\n");
#endif
    }
  } else {
#ifdef DEBUG
    Serial.printf("STA GW IP %s MyIP %s %ddbm\n", WiFi.gatewayIP().toString().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
#endif
  }
  digitalWrite(LED_BUILTIN, HIGH);
  configTime("IST-5:30", "pool.ntp.org", "time.nist.gov");
}

void loop() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  if (localtime_r(&now, &timeinfo)) {
    char tm[8];
    strftime(tm, sizeof(tm), "%H", &timeinfo);
#ifdef DEBUG
    Serial.println(tm);
#endif
    unsigned char ts = atoi(tm);
    if (ts == hr.toInt()) {
      if (!full && !motor) {
#ifdef DEBUG
        Serial.printf("MOTOR ON\n");
#endif
        motor = true;
        digitalWrite(LED_R, HIGH);
        digitalWrite(LED_G, LOW);
        for (int i = 0; i < 5; i++) {
          tone(BUZ, 1000, 1000);
          delay(1000);
          noTone(BUZ);
          delay(500);
        }
        digitalWrite(RELAY, HIGH);
      }
    } else {
      motor = false;
      full = false;
      digitalWrite(LED_R, LOW);
      digitalWrite(RELAY, LOW);
    }
  }

  if (digitalRead(DIN) && motor && !full) {
    motor = false;
    full = true;
    digitalWrite(LED_R, LOW);
    digitalWrite(RELAY, LOW);
#ifdef DEBUG
    Serial.printf("MOTOR OFF\n");
#endif
  }
#ifdef DEBUG
  Serial.printf("STATUS MOTOR:%d FULL:%d\n", motor, full);
#endif
  if (motor) digitalWrite(LED_R,digitalRead(LED_R) ^ 1);
  else {
    digitalWrite(LED_G, HIGH);
    delay(50);
    digitalWrite(LED_G, LOW);
    delay(100);
    digitalWrite(LED_G, HIGH);
    delay(50);
    digitalWrite(LED_G, LOW);
  }
  delay(5000);
}