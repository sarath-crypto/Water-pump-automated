#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <user_interface.h>
#include <PolledTimeout.h>
#include <include/WiFiState.h>
#include <EEPROM.h>
#include <Ticker.h>

#define PDU_MTU 512
#define BUF_LEN 1452
#define ADC_SZ 512

#define STA_RETRY 16
#define PORT 8880

#define AP_SSID "PUMPSYS_AP"
#define AP_PASS "PUMPSYS_PASS"

#define TRIG_PIN 4
#define ADC_PIN A0

//#define DEBUG 1

typedef struct pdu {
  unsigned char data[PDU_MTU];
  unsigned char len;
} pdu;

typedef struct ee_data {
  unsigned char ssid_len;
  char ssid[31];
  unsigned char pass_len;
  char pass[31];
  unsigned char key_len;
  char key[31];
} ee_data;

ee_data ee;

const char *PARAM_INPUT_1 = "input1";
const char *PARAM_INPUT_2 = "input2";
const char *PARAM_INPUT_3 = "input3";

const char s1[] PROGMEM = { "<!DOCTYPE HTML><html><head><title>PUMPYS Configuration</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body><form action=\"/get\">______WiFi_SSID:<input type=\"text\" name=\"input1\" value=\"" };
const char s2[] PROGMEM = { "\"><br><br>WiFi_PASSWORD:<input type=\"text\" name=\"input2\" value=\"" };
const char s3[] PROGMEM = { "\"><br><br>____________KEY:<input type=\"text\" name=\"input3\" value=\"" };
const char s4[] PROGMEM = { "\"><input type=\"submit\" value=\"Reboot\"></form></body></html>" };
String index_html;

String ssid;
String pass;
String key;

char trx[BUF_LEN];
void (*reset)(void) = 0;
unsigned char rxp = 0xff;
WiFiUDP Udp;
Ticker timer_ka;

void reboot(void) {
  rxp--;
}
void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Invalid Request");
}

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ADC_PIN, INPUT);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, HIGH);
#ifdef DEBUG
  Serial.begin(115200);
#endif
  EEPROM.begin(sizeof(ee_data));
  EEPROM.get(0, ee);
  ssid = String(ee.ssid);
  pass = String(ee.pass);
  key = String(ee.key);
#ifdef DEBUG
  Serial.printf("\nconfig: %s %s %s\n", ssid.c_str(), pass.c_str(), key.c_str());
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
#ifdef DEBUG
    Serial.printf("Connecting to AP %s %s %d %d\n", ssid.c_str(), pass.c_str(), WiFi.status(), retry);
#endif
    digitalWrite(BUILTIN_LED, HIGH);
    delay(250);
    digitalWrite(BUILTIN_LED, LOW);
    delay(250);
    retry++;
    if (retry >= STA_RETRY) break;
  }
  digitalWrite(BUILTIN_LED, LOW);
  if (retry >= STA_RETRY) {
    IPAddress apip(10, 10, 10, 1);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apip, apip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASS, 7, false, 1);

    index_html = String(s1) + ssid + String(s2) + pass + String(s3) + key + String(s4);
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
        key = request->getParam(PARAM_INPUT_3)->value();
      }
#ifdef DEBUG
      Serial.printf("Web message %s %s %s\n", ssid.c_str(), pass.c_str(), key.c_str());
#endif
      strcpy(ee.ssid, ssid.c_str());
      ee.ssid_len = ssid.length();
      strcpy(ee.pass, pass.c_str());
      ee.pass_len = pass.length();
      strcpy(ee.key, key.c_str());
      ee.key_len = key.length();
      EEPROM.put(0, ee);
      EEPROM.commit();

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
    Udp.begin(PORT);
  }
  digitalWrite(LED_BUILTIN, HIGH);
  timer_ka.attach(1, reboot);
}

void loop() {
  if (!rxp) reset();
  int pktsz = Udp.parsePacket();
  if (pktsz) {
    memset(&trx, 0, sizeof(trx));
    int n = Udp.read(trx, BUF_LEN);
    char buf[64];
    memcpy(buf, trx, n);
    buf[n] = '\0';
    String msg = String(buf);
#ifdef DEBUG
    Serial.printf("RX %s %s %d\n", Udp.remoteIP().toString().c_str(), msg.c_str(), n);
#endif
    if (msg == key) {
      rxp = 0xff;
      digitalWrite(LED_BUILTIN, LOW);
      tone(TRIG_PIN, 2000, 200);
      delay(5);
      unsigned int adc_data[ADC_SZ];
      for (int i = 0; i < ADC_SZ; i++) adc_data[i] = analogRead(ADC_PIN);
      float adc = 0;
      for (int i = 0; i < ADC_SZ; i++) adc += adc_data[i];
      adc = (float)adc / (float)ADC_SZ;

      pdu p;
      String data = key + " " + String(adc);
      memcpy(p.data, data.c_str(), data.length());
      p.len = data.length();
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write((char *)&p.data, p.len);
      Udp.endPacket();
      Udp.flush();
#ifdef DEBUG
      Serial.printf("TX server[%s] port[%d] len[%d] data[%s] %d\n", Udp.remoteIP().toString().c_str(), Udp.remotePort(), p.len, data.c_str(), rxp);
#endif
      digitalWrite(LED_BUILTIN, HIGH);
    }
  }
}
