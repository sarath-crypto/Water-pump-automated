#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <ESP8266WebServer.h>
#include <user_interface.h>
#include <include/WiFiState.h>
#include <time.h>
#include "LittleFS.h"

#define LED_R 12
#define LED_G 13
#define LED_W 16
#define BUZ 14
#define RELAY 5
#define RADIO_IN 4

#define AP_SSID "MOTOR_AP"
#define AP_PASS "MOTOR_PASS"
#define STA_RETRY 32

//#define DEBUG 1

String ssid;
String pass;
String nkey;
String okey;
String key;
String hr;
String epoch;

bool wrst = false;
bool ap_mode = false;
bool motor = false;
bool full = false;
time_t epochTime = 1784643900;
unsigned char ts = 0;
struct tm *timeInfo;
unsigned char sec = 0;

ESP8266WebServer server(80);
Ticker timer_sec;
void (*reset)(void) = 0;

void ticks(void) {
  epochTime++;
  sec++;
}

void control_io(void) {
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

  if (digitalRead(RADIO_IN) && motor && !full) {
    motor = false;
    full = true;
    digitalWrite(LED_R, LOW);
    digitalWrite(RELAY, LOW);
#ifdef DEBUG
    Serial.printf("MOTOR OFF\n");
#endif
  }
  if (motor) digitalWrite(LED_R, digitalRead(LED_R) ^ 1);
  else {
    unsigned char lp = LED_G;
    if (ap_mode) lp = LED_R;
    digitalWrite(lp, HIGH);
    delay(50);
    digitalWrite(lp, LOW);
    delay(100);
    digitalWrite(lp, HIGH);
    delay(50);
    digitalWrite(lp, LOW);
  }
}

void handle_reconfig() {
  ssid = server.arg(0);
  pass = server.arg(1);
  nkey = server.arg(2);
  okey = server.arg(3);
  hr = server.arg(4);
  epoch = server.arg(5);

#ifdef DEBUG
  Serial.printf("Reconfig[%d] %s %s %s %s %s %s\n", server.args(), ssid.c_str(), pass.c_str(), nkey.c_str(), okey.c_str(), hr.c_str(), epoch.c_str());
#endif
  if ((okey == key) || ap_mode) {
    File ofs = LittleFS.open("/config.txt", "w");
    if (ofs) {
      String msg = "ssid = " + ssid + "\npassword = " + pass + "\nkey = " + nkey + "\nhr = " + hr + "\nepoch = " + epoch;
      ofs.print(msg);
      ofs.close();
    }
    wrst = true;
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handle_root() {
  String msg;
  File ifs = LittleFS.open("/index.html", "r");
  if (ifs) {
    msg = ifs.readString();
    ifs.close();
  }
  server.send(200, "text/html", msg);
}

void listAllFiles(const char *dirname) {
  Serial.printf("\nListing directory: %s\n", dirname);
  Dir root = LittleFS.openDir(dirname);
  while (root.next()) {
    Serial.print("FILE: ");
    Serial.print(root.fileName());
    Serial.print("  SIZE: ");
    Serial.println(root.fileSize());
  }
}

String extractValue(String line) {
  int equalsIndex = line.indexOf('=');
  if (equalsIndex == -1) {
    return "";
  }
  String value = line.substring(equalsIndex + 1);
  value.trim();
  return value;
}


void setup() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_W, OUTPUT);
  pinMode(BUZ, OUTPUT);
  pinMode(RELAY, OUTPUT);
  pinMode(RADIO_IN, INPUT);

  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_W, LOW);
  digitalWrite(BUZ, LOW);
  digitalWrite(RELAY, LOW);

  digitalWrite(LED_R, HIGH);
  delay(1000);
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  delay(1000);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_W, HIGH);
  delay(1000);
  digitalWrite(LED_W, LOW);

  timer_sec.attach(1, ticks);
#ifdef DEBUG
  Serial.begin(115200);
#endif
  if (!LittleFS.begin()) {
    LittleFS.format();
    ap_mode = true;
  }

  if (!ap_mode) {
#ifdef DEBUG
    listAllFiles("/");
#endif

    File ifs = LittleFS.open("/config.txt", "r");
    if (ifs) {
      String line = ifs.readStringUntil('\n');
      ssid = extractValue(line);
      line = ifs.readStringUntil('\n');
      pass = extractValue(line);
      line = ifs.readStringUntil('\n');
      key = extractValue(line);
      line = ifs.readStringUntil('\n');
      hr = extractValue(line);
      line = ifs.readStringUntil('\n');
      epoch = extractValue(line);
#ifdef DEBUG
      Serial.printf("config: %s %s %s %s %s\n", ssid.c_str(), pass.c_str(), key.c_str(), hr.c_str(), epoch.c_str());
#endif
    } else ap_mode = true;
  }
  if (!ap_mode) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED) {
#ifdef DEBUG
      Serial.printf("Connecting to AP %s %s %d %d\n", ssid.c_str(), pass.c_str(), WiFi.status(), retry);
#endif
      digitalWrite(LED_G, HIGH);
      delay(250);
      digitalWrite(LED_G, LOW);
      delay(250);
      retry++;
      if (retry >= STA_RETRY) {
        ap_mode = true;
        break;
      }
    }
#ifdef DEBUG
    Serial.printf("STA GW IP %s MyIP %s %ddbm\n", WiFi.gatewayIP().toString().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
#endif
  }
  if (ap_mode) {
#ifdef DEBUG
    Serial.println("AP mode");
#endif
    IPAddress apip(10, 10, 10, 1);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apip, apip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASS, 7, false, 2);
    epochTime = epoch.toInt();
  }else configTime(19800, 0, "pool.ntp.org");

  server.on("/reconfig", HTTP_POST, handle_reconfig);
  server.on("/", HTTP_GET, handle_root);
  server.begin();
  wifi_set_sleep_type(LIGHT_SLEEP_T);
}


void loop() {
  if (ap_mode) {
    timeInfo = gmtime(&epochTime);
    ts = String(timeInfo->tm_hour).toInt();
  } else {
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (localtime_r(&now, &timeinfo)) {
      char tm[8];
      strftime(tm, sizeof(tm), "%H", &timeinfo);
      ts = atoi(tm);
    }
  }

  server.handleClient();
  if (wrst) {
#ifdef DEBUG
    Serial.printf("Web Reset\n");
#endif
    delay(1000);
    reset();
  }
  if (sec >= 5) {
#ifdef DEBUG
      Serial.printf("HOUR %d\n",ts);
#endif
    sec = 0;
    control_io();
  }
  if (digitalRead(RADIO_IN)) digitalWrite(LED_W, HIGH);
  else digitalWrite(LED_W, LOW);
}
