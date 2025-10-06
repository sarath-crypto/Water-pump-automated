#include <Arduino.h>
#include <FS.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Ticker.h>
#include <user_interface.h>
#include <PolledTimeout.h>
#include <include/WiFiState.h>

#define PDU_MTU     512
#define HEADER_LEN  2
#define BUF_LEN     1452

#define STA_RETRY 16
#define PORT      8880

#define AP_SSID "PWRSYS_AP"
#define AP_PASS "PWRSYS_PASS"

#define LED_R 12
#define LED_G 13
#define BUZ 14
#define TRIAC 16
#define RELAY 5

//#define FORMAT 1
//#define DEBUG 1

typedef struct pdu {
  unsigned char data[PDU_MTU];
  unsigned char len;
} pdu;

const char *PARAM_INPUT_1 = "input1";
const char *PARAM_INPUT_2 = "input2";
const char *PARAM_INPUT_3 = "input3";

const char s1[] PROGMEM = { "<!DOCTYPE HTML><html><head><title>PWRSYS Configuration</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body><form action=\"/get\">______WiFi_SSID:<input type=\"text\" name=\"input1\" value=\"" };
const char s2[] PROGMEM = { "\"><br><br>WiFi_PASSWORD:<input type=\"text\" name=\"input2\" value=\"" };
const char s3[] PROGMEM = { "\"><br><br>____________KEY:<input type=\"text\" name=\"input3\" value=\"" };
const char s4[] PROGMEM = { "\"><input type=\"submit\" value=\"Reboot\"></form></body></html>" };
String index_html;

String ssid;
String pass;
String key;
bool motor = false;

char trx[BUF_LEN];
void (*reset)(void) = 0;

WiFiUDP Udp;

String get_value(String line, String key) {
  int lpos = 0;
  String item;
  do {
    lpos = line.indexOf('\n');
    item = line.substring(0, lpos);
    int ipos = item.indexOf('=');
    String param = item.substring(0, ipos);
    if (param == key) {
      int vpos = item.indexOf('=');
      key = item.substring(vpos + 1);
      break;
    }
    lpos++;
    line = line.substring(lpos);
  } while (lpos);
  return key;
}

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Invalid Request");
}

void setup() {
#ifdef DEBUG
  Serial.begin(115200);
#endif
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(BUZ, OUTPUT);
  pinMode(TRIAC, OUTPUT);
  pinMode(RELAY, OUTPUT);

  digitalWrite(LED_R, HIGH);
  SPIFFS.begin();
  delay(1000);

#ifdef FORMAT
  SPIFFS.format();
#ifdef DEBUG
  Serial.printf("\nFormat completed\n");
#endif
#endif
  Dir dir = SPIFFS.openDir("/");
  File fin = SPIFFS.open("/config.txt", "r");
  if (fin) {
    String line;
    while (fin.available()) {
      char c = fin.read();
      if ((c == '\n') || (c == '\r')) {
        if (line[line.length() - 1] != '\n') line += '\n';
        continue;
      }
      if (c != ' ') line += c;
    }
    fin.close();
    ssid = get_value(line, "ssid");
    pass = get_value(line, "password");
    key = get_value(line, "key");
  }
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
      File fout = SPIFFS.open("/config.txt", "w");
      if (!fout) reset();
      ssid = "ssid = " + ssid;
      pass = "password = " + pass;
      key = "key = " + key;
      fout.println(ssid.c_str());
      fout.println(pass.c_str());
      fout.println(key.c_str());

      fout.close();
      request->redirect("http://10.10.10.1");
      reset();
    });
    server.onNotFound(notFound);
    server.begin();

    while (1) {
      digitalWrite(LED_R, HIGH);
      digitalWrite(LED_G, LOW);
      delay(500);
      digitalWrite(LED_R, LOW);
      digitalWrite(LED_G, HIGH);
      delay(500);
#ifdef DEBUG
      Serial.printf("AP mode webserver waiting\n");
#endif
    }
  } else {
#ifdef DEBUG
    Serial.printf("AP GW IP %s MyIP %s %ddbm\n", WiFi.gatewayIP().toString().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
#endif
    Udp.begin(PORT);
  }
}

void loop() {
  int pktsz = Udp.parsePacket();
  if (pktsz) {
    memset(&trx, 0, sizeof(trx));
    int n = Udp.read(trx, BUF_LEN);
    pdu *prx = (pdu *)&trx;
    char buf[64];
    memcpy(buf, prx->data, prx->len - HEADER_LEN);
    buf[prx->len - HEADER_LEN] = '\0';
    String msg = String(buf);
#ifdef DEBUG
    Serial.printf("RX %s %d %s\n", Udp.remoteIP().toString().c_str(), (int)prx->len, msg.c_str());
#endif
    int index = msg.indexOf(" ");
    String cmd = msg.substring(index + 1);
    msg.remove(index, msg.length() - index);
#ifdef DEBUG
    Serial.printf("Receive %d %s:%d %s:%d %s %d\n", index, msg.c_str(), msg.length(), cmd.c_str(), cmd.length(), key.c_str(), motor);
#endif
    if (msg == key) {
      if (!motor) digitalWrite(LED_G, digitalRead(LED_G) ^ 1);
      else digitalWrite(LED_R, digitalRead(LED_R) ^ 1);

      if ((cmd == "ON") && (!motor)) {
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
        digitalWrite(TRIAC, HIGH);
      } else if ((cmd == "OFF") && (motor)) {
        digitalWrite(LED_R, LOW);
        motor = false;
        digitalWrite(RELAY, LOW);
        digitalWrite(TRIAC, LOW);
      }

      pdu p;
      String data = key + " " + cmd;
      memcpy(p.data, data.c_str(), data.length());
      p.len = data.length();
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write((char *)&p.data, p.len);
      Udp.endPacket();
      Udp.flush();
#ifdef DEBUG
      Serial.printf("TX server[%s] port[%d] len[%d] data[%s]\n", Udp.remoteIP().toString().c_str(), Udp.remotePort(), p.len, data.c_str());
#endif
    }
    Udp.flush();
  }
}
