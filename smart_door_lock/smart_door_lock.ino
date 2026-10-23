// Smart Door Lock - ESP32 with keypad, OLED, PIR sensor, servo and MQTT control.
// Reconstructed from my FH CAMPUS 02 elective-project presentation (original sketch lost).
// Pin numbers and the web page are my re-implementation: check them against your wiring.
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>   // MQTT communication
#include <ESP32Servo.h>     // control the servo motor
#include <Keypad.h>         // 4x4 keypad input
#include <Wire.h>           // I2C OLED support
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"        // copy secrets.example.h to secrets.h and edit

// ---- Pins (adjust to your wiring) ----
const int SERVO_PIN = 18;
const int PIR_PIN   = 19;                       // OLED uses default I2C: SDA 21, SCL 22
byte rowPins[4] = {13, 12, 14, 27};
byte colPins[4] = {26, 25, 33, 32};

// ---- Settings ----
const int SERVO_LOCKED = 90, SERVO_OPEN = 0;
const unsigned long AUTO_LOCK_MS = 10000;       // relock after 10 s
const unsigned long MOTION_LOG_GAP_MS = 5000;   // do not spam the log
const int SCREEN_WIDTH = 128, SCREEN_HEIGHT = 64, OLED_RESET = -1;

// Wi-Fi Credentials (kept in secrets.h, not in the repository)
const char* ssid = WIFI_SSID;
const char* wifiPassword = WIFI_PASSWORD;

// MQTT Configuration
const char* mqttServer = "mqtt.croku.at";
const int mqttPort = 1883;                      // plain text; see README for the TLS (8883) upgrade

// MQTT Topics
const char* statusTopic  = "/lock/status";
const char* logTopic     = "/lock/logs";
const char* commandTopic = "/lock/command";

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Servo servo;

char keys[4][4] = {{'1','2','3','A'},{'4','5','6','B'},{'7','8','9','C'},{'*','0','#','D'}};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, 4, 4);

// ---- Lock state ----
bool isLocked = true;
String correctPIN = DEFAULT_PIN;   // "1234" in the original project
String enteredPIN = "";
unsigned long unlockedAt = 0, lastMotionLog = 0, lastMqttTry = 0;

// ---- Display ----
void displayEnterPrompt() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Enter PIN:");
  display.setCursor(0, 16);
  for (unsigned i = 0; i < enteredPIN.length(); i++) display.print('*');
  display.display();
}

void displayMessage(const String& msg, int ms) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(msg);
  display.display();
  if (ms > 0) delay(ms);
}

// ---- Event logger ----
void logEvent(const String& msg) {
  Serial.println("LOG: " + msg);
  if (client.connected()) client.publish(logTopic, msg.c_str());
}

// ---- Door control ----
void lockDoor() {
  servo.write(SERVO_LOCKED);  // Locked
  isLocked = true;
  displayMessage("Door Locked", 1000);
  displayEnterPrompt();
  if (client.connected()) client.publish(statusTopic, "locked");
  logEvent("Door locked");
}

void unlockDoor() {
  servo.write(SERVO_OPEN);  // Open
  isLocked = false;
  unlockedAt = millis();
  displayMessage("Door Open", 0);
  delay(500);
  if (client.connected()) client.publish(statusTopic, "unlocked");
  logEvent("Door unlocked");

  display.setCursor(0, 8);
  display.println("Waiting...");
  display.display();
}

// ---- MQTT callback ----
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  Serial.printf("MQTT message received on topic [%s]: %s\n", topic, message.c_str());

  if (String(topic) == commandTopic) {
    if (message == "unlock") {
      Serial.println("Received 'unlock' command");
      unlockDoor();
    } else if (message == "lock") {
      Serial.println("Received 'lock' command");
      lockDoor();
    } else {
      Serial.println("Unknown command received");
    }
  }
}

void mqttReconnect() {  // non-blocking: one try every 5 s
  if (client.connected() || millis() - lastMqttTry < 5000) return;
  lastMqttTry = millis();
  String id = "smartlock-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (client.connect(id.c_str())) {
    client.subscribe(commandTopic);
    client.publish(statusTopic, isLocked ? "locked" : "unlocked");
  }
}

// ---- Web interface (reachable on the ESP32 access point, 192.168.4.1) ----
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Smart Lock</title></head><body>";
  html += String("<h2>Smart Lock Status: ") + (isLocked ? "&#128274; LOCKED" : "&#128275; UNLOCKED") + "</h2>";
  html += "<form action='/unlock' method='POST'><button>Unlock</button></form><br>";
  html += "<form action='/lock' method='POST'><button>Lock</button></form><br>";
  html += "<form action='/setpin' method='POST'>New PIN: <input name='pin' type='password' maxlength='8'> <button>Set PIN</button></form>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void redirectHome() { server.sendHeader("Location", "/"); server.send(303); }

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/unlock", HTTP_POST, []() { unlockDoor(); redirectHome(); });
  server.on("/lock", HTTP_POST, []() { lockDoor(); redirectHome(); });
  server.on("/setpin", HTTP_POST, []() {
    String p = server.arg("pin");
    bool ok = p.length() >= 4 && p.length() <= 8;
    for (unsigned i = 0; ok && i < p.length(); i++) ok = isDigit(p[i]);
    if (ok) { correctPIN = p; logEvent("PIN changed"); } else logEvent("PIN change rejected");
    redirectHome();
  });
  server.begin();
}

// ---- Keypad PIN entry ----
void handleKeypad() {
  char k = keypad.getKey();
  if (!k) return;
  if (k == '#') {                         // submit
    if (enteredPIN == correctPIN) {
      logEvent("PIN accepted");
      unlockDoor();
    } else {
      logEvent("Wrong PIN attempt");
      displayMessage("Wrong PIN", 1000);
      displayEnterPrompt();
    }
    enteredPIN = "";
    if (isLocked) displayEnterPrompt();
  } else if (k == '*') {                  // clear
    enteredPIN = "";
    displayEnterPrompt();
  } else if (isDigit(k) && enteredPIN.length() < 8) {
    enteredPIN += k;
    displayEnterPrompt();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  servo.attach(SERVO_PIN);
  servo.write(SERVO_LOCKED);

  if (!display.begin(SSD1306_SWITCHVCC, 0x3C)) Serial.println("OLED not found");
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();   // clearDisplay() fixed the mixed OLED messages
  displayMessage("Starting...", 0);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);      // local control page at 192.168.4.1
  WiFi.begin(ssid, wifiPassword);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  setupWeb();
  displayEnterPrompt();
}

void loop() {
  mqttReconnect();
  client.loop();
  server.handleClient();
  handleKeypad();

  if (digitalRead(PIR_PIN) && millis() - lastMotionLog > MOTION_LOG_GAP_MS) {
    lastMotionLog = millis();
    logEvent("Motion detected");
  }
  if (!isLocked && millis() - unlockedAt > AUTO_LOCK_MS) lockDoor();   // auto-lock timeout
}
