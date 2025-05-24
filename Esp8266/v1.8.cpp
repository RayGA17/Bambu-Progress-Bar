#include <Arduino.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <time.h>

// LED strip configuration
#define LED_PIN D4
#define LED_COUNT 20
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// MQTT server details
const char* mqttServer = "cn.mqtt.bambulab.com";
const int mqttPort = 8883;

// Global objects
WiFiManager wm;
ESP8266WebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// Configuration variables
char uid[32] = "";
char accessToken[256] = "";
char deviceID[32] = "";
int globalBrightness = 50;
char standbyMode[10] = "marquee";
bool overlayMarquee = false;
uint32_t progressBarColor = strip.Color(0, 255, 0); // Green
uint32_t standbyBreathingColor = strip.Color(0, 0, 255); // Blue
float progressBarBrightnessRatio = 1.0;
float standbyBrightnessRatio = 1.0;
String mqttTopicSub = "device/{DEVICE_ID}/report";
String mqttTopicPub = "device/{DEVICE_ID}/request";

// Timing control
unsigned long lastReconnectAttempt = 0;
const long reconnectInterval = 5000;
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 10000;
unsigned long lastPushallTime = 0;
const unsigned long pushallInterval = 30000; // 30 seconds

// LED animation control
unsigned long lastLedUpdate = 0;
const long ledUpdateInterval = 33;
int marqueePosition = 0;
bool apClientConnected = false;

// LED test state machine
bool testingLed = false;
int testLedIndex = 0;
unsigned long lastTestLedUpdate = 0;
const long testLedInterval = 50;

// Single device web access control
IPAddress activeClientIP(0, 0, 0, 0);
unsigned long activeClientTimeout = 0;
const long clientTimeoutInterval = 60000;

// State enumeration
enum State {
  AP_MODE,
  CONNECTING_WIFI,
  CONNECTED_WIFI,
  CONNECTING_PRINTER,
  CONNECTED_PRINTER,
  PRINTING,
  ERROR
};
State currentState = AP_MODE;
int printPercent = 0;
String gcodeState = "";
String mqttError = "";
int remainingTime = 0;
int layerNum = 0;

// Global printer state storage
StaticJsonDocument<1024> printerState;

// Function declarations
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void loadConfig();
void saveConfig();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void sendPushall();
uint32_t getRainbowColor(float position);
void updateLED();
void updateTestLed();
bool checkClientAccess();
void handleRoot();
void handleConfig();
void handleTestLed();
void handleLog();
void handleClearCache();
void handleResetConfig();
void handleSwitchMode();
bool isConfigValid();
bool isPrinting();

bool isConfigValid() {
  return strlen(uid) > 0 && strlen(accessToken) > 0 && strlen(deviceID) > 0;
}

bool isPrinting() {
  return (gcodeState == "RUNNING") || (remainingTime > 0) || (printPercent > 0) || (layerNum > 0);
}

void setup() {
  Serial.begin(115200);

  // Initialize LED strip
  strip.begin();
  strip.setBrightness(globalBrightness);
  bool ledOk = true;
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 255));
    strip.show();
    delay(10);
    strip.clear();
    strip.show();
    if (!strip.getPixelColor(i)) {
      ledOk = false;
      break;
    }
  }
  if (!ledOk) {
    Serial.println("LED strip initialization failed");
  } else {
    Serial.println("LED strip initialized successfully");
  }

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
    return;
  }

  // Load configuration
  loadConfig();

  // Configure WiFiManager
  WiFiManagerParameter custom_uid("uid", "UID", uid, 32);
  WiFiManagerParameter custom_access_token("accessToken", "Access Token", accessToken, 256);
  WiFiManagerParameter custom_device_id("deviceID", "Device Serial Number", deviceID, 32);
  wm.addParameter(&custom_uid);
  wm.addParameter(&custom_access_token);
  wm.addParameter(&custom_device_id);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);

  // Attempt WiFi connection
  currentState = CONNECTING_WIFI;
  if (!wm.autoConnect("BambuAP")) {
    Serial.println("WiFi connection failed, entering AP mode");
    currentState = AP_MODE;
  } else {
    Serial.println("WiFi connected successfully");
    currentState = CONNECTED_WIFI;
    strncpy(uid, custom_uid.getValue(), sizeof(uid) - 1);
    uid[sizeof(uid) - 1] = '\0';
    strncpy(accessToken, custom_access_token.getValue(), sizeof(accessToken) - 1);
    accessToken[sizeof(accessToken) - 1] = '\0';
    strncpy(deviceID, custom_device_id.getValue(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    saveConfig();

    // Synchronize time with NTP
    configTime(0, 0, "pool.ntp.org");
    time_t now = time(nullptr);
    while (now < 1000000000) {
      delay(500);
      now = time(nullptr);
    }
    Serial.println("Time synchronized");
  }

  // Enable watchdog
  ESP.wdtEnable(10000); // 10 seconds timeout

  // Configure MQTT
  if (isConfigValid()) {
    espClient.setInsecure();
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
  } else {
    Serial.println("Configuration empty, skipping MQTT initialization");
    mqttError = "Configuration empty, please set UID, Access Token, and Device Serial Number";
  }

  // Start web server
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/testLed", handleTestLed);
  server.on("/log", handleLog);
  server.on("/clearCache", handleClearCache);
  server.on("/resetConfig", handleResetConfig);
  server.on("/switchMode", handleSwitchMode);
  server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  ESP.wdtFeed(); // Feed watchdog
  unsigned long currentMillis = millis();
  server.handleClient();

  // Check WiFi status
  if (currentState != AP_MODE && currentMillis - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, attempting to reconnect");
      currentState = CONNECTING_WIFI;
      wm.startConfigPortal("BambuAP");
      if (WiFi.status() == WL_CONNECTED) {
        currentState = CONNECTED_WIFI;
        configTime(0, 0, "pool.ntp.org");
        time_t now = time(nullptr);
        while (now < 1000000000) {
          delay(500);
          now = time(nullptr);
        }
        Serial.println("Time synchronized");
      } else {
        currentState = AP_MODE;
      }
    }
  }

  // Handle MQTT connection
  if (currentState != AP_MODE && WiFi.status() == WL_CONNECTED && isConfigValid()) {
    if (!mqttClient.connected()) {
      currentState = CONNECTING_PRINTER;
      reconnectMQTT();
    } else {
      mqttClient.loop();
    }
  }

  // Send pushall periodically
  if (currentState == CONNECTED_PRINTER && currentMillis - lastPushallTime > pushallInterval) {
    sendPushall();
    lastPushallTime = currentMillis;
  }

  // Handle web client timeout
  if (activeClientIP != IPAddress(0, 0, 0, 0) && currentMillis - activeClientTimeout > clientTimeoutInterval) {
    activeClientIP = IPAddress(0, 0, 0, 0);
    Serial.println("Web client timed out, lock released");
  }

  // Update LEDs
  updateLED();
  updateTestLed();

  // Monitor heap memory
  if (ESP.getFreeHeap() < 10000) {
    Serial.println("Warning: Low heap memory, only " + String(ESP.getFreeHeap()) + " bytes remaining");
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  currentState = AP_MODE;
  apClientConnected = false;
  Serial.println("Entered AP mode");
}

void saveConfigCallback() {
  saveConfig();
}

void loadConfig() {
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("No config file found, using defaults");
    return;
  }
  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file too large");
    configFile.close();
    return;
  }
  char buf[1024];
  configFile.readBytes(buf, size);
  configFile.close();
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, buf);
  if (error) {
    Serial.println("Failed to parse config file: " + String(error.c_str()));
    return;
  }
  strncpy(uid, doc["uid"] | "", sizeof(uid) - 1);
  uid[sizeof(uid) - 1] = '\0';
  strncpy(accessToken, doc["accessToken"] | "", sizeof(accessToken) - 1);
  accessToken[sizeof(accessToken) - 1] = '\0';
  strncpy(deviceID, doc["deviceID"] | "", sizeof(deviceID) - 1);
  deviceID[sizeof(deviceID) - 1] = '\0';
  globalBrightness = doc["brightness"] | 50;
  strncpy(standbyMode, doc["standbyMode"] | "marquee", sizeof(standbyMode) - 1);
  standbyMode[sizeof(standbyMode) - 1] = '\0';
  overlayMarquee = doc["overlayMarquee"] | false;
  progressBarColor = doc["progressBarColor"] | strip.Color(0, 255, 0);
  standbyBreathingColor = doc["standbyBreathingColor"] | strip.Color(0, 0, 255);
  progressBarBrightnessRatio = doc["progressBarBrightnessRatio"] | 1.0;
  standbyBrightnessRatio = doc["standbyBrightnessRatio"] | 1.0;
  strip.setBrightness(constrain(globalBrightness, 0, 255));
}

void saveConfig() {
  StaticJsonDocument<1024> doc;
  doc["uid"] = uid;
  doc["accessToken"] = accessToken;
  doc["deviceID"] = deviceID;
  doc["brightness"] = globalBrightness;
  doc["standbyMode"] = standbyMode;
  doc["overlayMarquee"] = overlayMarquee;
  doc["progressBarColor"] = progressBarColor;
  doc["standbyBreathingColor"] = standbyBreathingColor;
  doc["progressBarBrightnessRatio"] = progressBarBrightnessRatio;
  doc["standbyBrightnessRatio"] = standbyBrightnessRatio;
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to write config file");
    return;
  }
  serializeJson(doc, configFile);
  configFile.close();
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    String clientID = "ESP8266Client-" + String(random(0xffff), HEX);
    Serial.println("Connecting to MQTT server: " + String(mqttServer));
    if (mqttClient.connect(clientID.c_str(), uid, accessToken)) {
      String topicSub = mqttTopicSub;
      topicSub.replace("{DEVICE_ID}", deviceID);
      mqttClient.subscribe(topicSub.c_str());
      currentState = CONNECTED_PRINTER;
      mqttError = "";
      Serial.println("MQTT connected successfully");
      sendPushall();
    } else {
      mqttError = "MQTT connection failed, error code=" + String(mqttClient.state());
      Serial.println(mqttError);
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("JSON parsing failed: " + String(error.c_str()));
    return;
  }
  if (doc.containsKey("print")) {
    JsonObject printData = doc["print"];
    if (doc.containsKey("command") && doc["command"] == "pushall") {
      printerState.clear();
      for (JsonPair kv : printData) {
        printerState[kv.key()] = kv.value();
      }
      Serial.println("Full update completed");
    } else {
      for (JsonPair kv : printData) {
        printerState[kv.key()] = kv.value();
      }
      Serial.println("Incremental update completed");
    }

    if (printerState.containsKey("gcode_state")) {
      gcodeState = printerState["gcode_state"].as<String>();
    }
    if (printerState.containsKey("mc_percent")) {
      printPercent = printerState["mc_percent"].as<int>();
    }
    if (printerState.containsKey("mc_remaining_time")) {
      remainingTime = printerState["mc_remaining_time"].as<int>();
    }
    if (printerState.containsKey("layer_num")) {
      layerNum = printerState["layer_num"].as<int>();
    }

    if (isPrinting()) {
      currentState = PRINTING;
    } else if (gcodeState == "FAILED") {
      currentState = ERROR;
    } else {
      currentState = CONNECTED_PRINTER;
    }

    Serial.print("Print state: " + gcodeState + ", Progress: " + String(printPercent));
  }
}

void sendPushall() {
  StaticJsonDocument<256> pushall_request;
  pushall_request["pushing"]["sequence_id"] = String(time(NULL));
  pushall_request["pushing"]["command"] = "pushall";
  pushall_request["pushing"]["version"] = 1;
  pushall_request["pushing"]["push_target"] = 1;
  String payload;
  serializeJson(pushall_request, payload);
  String topicPub = mqttTopicPub;
  topicPub.replace("{DEVICE_ID}", deviceID);
  mqttClient.publish(topicPub.c_str(), payload.c_str());
  Serial.println("Sent pushall request");
}

uint32_t getRainbowColor(float position) {
  position = fmod(position, 1.0);
  if (position < 0.166) return strip.Color(255, 0, 255 * (position / 0.166));
  else if (position < 0.333) return strip.Color(255 * (1 - (position - 0.166) / 0.166), 0, 255);
  else if (position < 0.5) return strip.Color(0, 255 * ((position - 0.333) / 0.166), 255);
  else if (position < 0.666) return strip.Color(0, 255, 255 * (1 - (position - 0.5) / 0.166));
  else if (position < 0.833) return strip.Color(255 * ((position - 0.666) / 0.166), 255, 0);
  else return strip.Color(255, 255 * (1 - (position - 0.833) / 0.166), 0);
}

void updateLED() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < ledUpdateInterval) return;
  lastUpdate = millis();
  if (testingLed) return;
  strip.clear();

  if (currentState == AP_MODE) {
    apClientConnected = WiFi.softAPgetStationNum() > 0;
    strip.setPixelColor(0, apClientConnected ? strip.Color(0, 255, 0) : ((millis() / 500) % 2 ? strip.Color(255, 255, 0) : strip.Color(0, 255, 0)));
  } else if (currentState == CONNECTING_WIFI || currentState == CONNECTING_PRINTER) {
    strip.setPixelColor(0, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : strip.Color(0, 0, 255));
  } else if (currentState == CONNECTED_WIFI) {
    strip.setPixelColor(0, strip.Color(0, 0, 255));
    strip.setPixelColor(1, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : 0);
  } else if (currentState == CONNECTED_PRINTER || currentState == PRINTING) {
    if (isPrinting()) {
      float pixels = printPercent * LED_COUNT / 100.0;
      int fullPixels = floor(pixels);
      float partialPixel = pixels - fullPixels;
      for (int i = 0; i < fullPixels; i++) {
        strip.setPixelColor(i, progressBarColor);
      }
      if (fullPixels < LED_COUNT) {
        strip.setPixelColor(fullPixels, progressBarColor & 0xFFFFFF | (int)(partialPixel * 255) << 24);
      }
      strip.setBrightness(globalBrightness * progressBarBrightnessRatio);
      if (overlayMarquee) {
        for (int i = 0; i < LED_COUNT; i++) {
          float pos = (float)(i + marqueePosition) / LED_COUNT;
          strip.setPixelColor(i, getRainbowColor(pos));
        }
        marqueePosition = (marqueePosition + 1) % LED_COUNT;
      }
    } else {
      if (strcmp(standbyMode, "marquee") == 0) {
        for (int i = 0; i < LED_COUNT; i++) {
          float pos = (float)(i + marqueePosition) / LED_COUNT;
          strip.setPixelColor(i, getRainbowColor(pos));
        }
        marqueePosition = (marqueePosition + 1) % LED_COUNT;
      } else if (strcmp(standbyMode, "breathing") == 0) {
        float brightness = (sin(millis() / 1000.0 * PI) + 1) / 2.0;
        int scaledBrightness = brightness * globalBrightness * standbyBrightnessRatio;
        for (int i = 0; i < LED_COUNT; i++) {
          strip.setPixelColor(i, standbyBreathingColor);
        }
        strip.setBrightness(scaledBrightness);
      }
    }
  } else if (currentState == ERROR) {
    for (int i = 0; i < LED_COUNT; i++) {
      strip.setPixelColor(i, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : 0);
    }
  }
  strip.show();
}

void updateTestLed() {
  if (!testingLed) return;
  if (millis() - lastTestLedUpdate < testLedInterval) return;
  lastTestLedUpdate = millis();
  strip.clear();
  if (testLedIndex < LED_COUNT) {
    strip.setPixelColor(testLedIndex, strip.Color(255, 255, 255));
    strip.show();
    testLedIndex++;
  } else {
    testingLed = false;
    testLedIndex = 0;
    strip.clear();
    strip.show();
  }
}

bool checkClientAccess() {
  IPAddress clientIP = server.client().remoteIP();
  if (activeClientIP == IPAddress(0, 0, 0, 0)) {
    activeClientIP = clientIP;
    activeClientTimeout = millis();
    Serial.println("New client connected: " + clientIP.toString());
    return true;
  } else if (clientIP == activeClientIP) {
    activeClientTimeout = millis();
    return true;
  } else {
    Serial.println("Rejected client " + clientIP.toString());
    return false;
  }
}

void handleRoot() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>Access Denied</h1><p>Another device is configuring.</p>");
    return;
  }

  String html;
  html.reserve(4096);
  html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "<title>Bambu Printer Status Display</title>"
           "<style>"
           "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f4; }"
           "h1 { color: #333; }"
           ".container { max-width: 600px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
           "label { display: block; margin: 10px 0 5px; color: #555; }"
           "input, select { width: 100%; padding: 8px; margin-bottom: 10px; border: 1px solid #ccc; border-radius: 4px; }"
           "button { background-color: #007bff; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; }"
           "button:hover { background-color: #0056b3; }"
           ".log { margin-top: 20px; padding: 10px; background: #e9ecef; border-radius: 4px; white-space: pre-wrap; }"
           "</style>"
           "<script>"
           "function validateForm() {"
           "  var uid = document.forms['configForm']['uid'].value;"
           "  var accessToken = document.forms['configForm']['accessToken'].value;"
           "  var deviceID = document.forms['configForm']['deviceID'].value;"
           "  if (!uid.trim() || !accessToken.trim() || !deviceID.trim()) {"
           "    alert('UID, Access Token, and Device Serial Number cannot be empty!');"
           "    return false;"
           "  }"
           "  var brightness = document.forms['configForm']['brightness'].value;"
           "  if (brightness < 0 || brightness > 255) {"
           "    alert('Brightness must be between 0 and 255!');"
           "    return false;"
           "  }"
           "  return true;"
           "}"
           "function updateLog() {"
           "  var xhr = new XMLHttpRequest();"
           "  xhr.open('GET', '/log', true);"
           "  xhr.onreadystatechange = function() {"
           "    if (xhr.readyState == 4 && xhr.status == 200) {"
           "      document.getElementById('log').innerHTML = xhr.responseText;"
           "    }"
           "  };"
           "  xhr.send();"
           "}"
           "setInterval(updateLog, 5000);"
           "</script></head><body>"
           "<div class='container'>"
           "<h1>Bambu Printer Status Display</h1>"
           "<p>Device Status: ");

  switch (currentState) {
    case AP_MODE: html += "AP Mode"; break;
    case CONNECTING_WIFI: html += "Connecting to WiFi"; break;
    case CONNECTED_WIFI: html += "WiFi Connected"; break;
    case CONNECTING_PRINTER: html += "Connecting to Printer"; break;
    case CONNECTED_PRINTER: html += "Printer Connected"; break;
    case PRINTING: html += "Printing (Progress: " + String(printPercent) + "%)"; break;
    case ERROR: html += "Print Error"; break;
  }
  html += "</p>";

  html += "<form name='configForm' action='/config' method='POST' onsubmit='return validateForm()'>"
          "<label>UID: <input type='text' name='uid' value='" + String(uid) + "'></label>"
          "<label>Access Token: <input type='text' name='accessToken' value='" + String(accessToken) + "' maxlength='256'></label>"
          "<label>Device Serial Number: <input type='text' name='deviceID' value='" + String(deviceID) + "'></label>"
          "<label>Global Brightness (0-255): <input type='number' name='brightness' min='0' max='255' value='" + String(globalBrightness) + "'></label>"
          "<label>Standby Mode: <select name='standbyMode'>"
          "<option value='marquee'" + (strcmp(standbyMode, "marquee") == 0 ? " selected" : "") + ">Rainbow Marquee</option>"
          "<option value='breathing'" + (strcmp(standbyMode, "breathing") == 0 ? " selected" : "") + ">Breathing Light</option>"
          "</select></label>"
          "<label>Progress Bar Color: <input type='color' name='progressBarColor' value='#" + String(progressBarColor, HEX) + "'></label>"
          "<label>Standby Breathing Color: <input type='color' name='standbyBreathingColor' value='#" + String(standbyBreathingColor, HEX) + "'></label>"
          "<label>Progress Bar Brightness Ratio (0.0-1.0): <input type='number' step='0.1' min='0' max='1' name='progressBarBrightnessRatio' value='" + String(progressBarBrightnessRatio) + "'></label>"
          "<label>Standby Brightness Ratio (0.0-1.0): <input type='number' step='0.1' min='0' max='1' name='standbyBrightnessRatio' value='" + String(standbyBrightnessRatio) + "'></label>"
          "<label><input type='checkbox' name='overlayMarquee'" + (overlayMarquee ? " checked" : "") + "> Overlay Marquee on Progress</label>"
          "<button type='submit'>Save Configuration</button>"
          "</form>";

  html += "<form action='/testLed' method='POST'><button type='submit'>Test LED Strip</button></form>"
          "<form action='/clearCache' method='POST'><button type='submit'>Clear Cache</button></form>"
          "<form action='/resetConfig' method='POST'><button type='submit'>Reset Configuration</button></form>"
          "<form action='/switchMode' method='POST'>"
          "<label>Switch to: <select name='mode'>"
          "<option value='progress'>Progress Bar</option>"
          "<option value='standby'>Standby</option>"
          "</select></label><button type='submit'>Switch Mode</button></form>"
          "<div class='log' id='log'>Loading logs...</div></div>"
          "<script>updateLog();</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleConfig() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>Access Denied</h1><p>Another device is configuring.</p>");
    return;
  }

  if (server.hasArg("uid") && server.hasArg("accessToken") && server.hasArg("deviceID")) {
    strncpy(uid, server.arg("uid").c_str(), sizeof(uid) - 1);
    uid[sizeof(uid) - 1] = '\0';
    strncpy(accessToken, server.arg("accessToken").c_str(), sizeof(accessToken) - 1);
    accessToken[sizeof(accessToken) - 1] = '\0';
    strncpy(deviceID, server.arg("deviceID").c_str(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    globalBrightness = server.arg("brightness").toInt();
    strncpy(standbyMode, server.arg("standbyMode").c_str(), sizeof(standbyMode) - 1);
    standbyMode[sizeof(standbyMode) - 1] = '\0';
    overlayMarquee = server.hasArg("overlayMarquee") && server.arg("overlayMarquee") == "on";
    progressBarColor = strtoul(server.arg("progressBarColor").substring(1).c_str(), NULL, 16);
    standbyBreathingColor = strtoul(server.arg("standbyBreathingColor").substring(1).c_str(), NULL, 16);
    progressBarBrightnessRatio = server.arg("progressBarBrightnessRatio").toFloat();
    standbyBrightnessRatio = server.arg("standbyBrightnessRatio").toFloat();
    strip.setBrightness(constrain(globalBrightness, 0, 255));
    saveConfig();
    if (isConfigValid()) {
      mqttClient.setServer(mqttServer, mqttPort);
      currentState = CONNECTING_PRINTER;
      sendPushall(); // Request full package
    }
    activeClientIP = IPAddress(0, 0, 0, 0);
    server.send(200, "text/html", "<h1>Configuration Saved</h1><p><a href='/'>Return</a></p>");
  } else {
    server.send(400, "text/html", "<h1>Missing Parameters</h1><p><a href='/'>Return</a></p>");
  }
}

void handleTestLed() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>Access Denied</h1><p>Another device is configuring.</p>");
    return;
  }
  testingLed = true;
  testLedIndex = 0;
  lastTestLedUpdate = millis();
  server.send(200, "text/html", "<h1>Starting LED Test</h1><p><a href='/'>Return</a></p>");
}

void handleLog() {
  String logContent = "WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "<br>"
                      "Config Status: " + String(isConfigValid() ? "Valid" : "Invalid") + "<br>"
                      "MQTT Status: " + String(mqttClient.connected() ? "Connected" : mqttError) + "<br>"
                      "Printer State: " + gcodeState + "<br>"
                      "Print Progress: " + String(printPercent) + "%<br>"
                      "Remaining Time: " + String(remainingTime) + " minutes<br>"
                      "Layer Number: " + String(layerNum) + "<br>"
                      "Nozzle Temp: " + String(printerState["nozzle_temper"].as<float>()) + "°C<br>"
                      "Bed Temp: " + String(printerState["bed_temper"].as<float>()) + "°C<br>"
                      "LED Mode: " + String(currentState == PRINTING ? "Progress Bar" : "Standby") + "<br>"
                      "Progress Bar Color: #" + String(progressBarColor, HEX) + "<br>"
                      "Standby Breathing Color: #" + String(standbyBreathingColor, HEX) + "<br>"
                      "Progress Bar Brightness Ratio: " + String(progressBarBrightnessRatio) + "<br>"
                      "Standby Brightness Ratio: " + String(standbyBrightnessRatio) + "<br>"
                      "Overlay Marquee: " + String(overlayMarquee ? "Enabled" : "Disabled") + "<br>"
                      "Free Heap: " + String(ESP.getFreeHeap()) + " bytes";
  server.send(200, "text/html", logContent);
}

void handleClearCache() {
  server.send(200, "text/html", "<h1>Cache Cleared</h1><p><a href='/'>Return</a></p>");
}

void handleResetConfig() {
  LittleFS.remove("/config.json");
  ESP.restart();
}

void handleSwitchMode() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>Access Denied</h1><p>Another device is configuring.</p>");
    return;
  }
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "progress") {
      currentState = PRINTING;
    } else if (mode == "standby") {
      currentState = CONNECTED_PRINTER;
    }
    server.send(200, "text/html", "<h1>Mode Switched to " + mode + "</h1><p><a href='/'>Return</a></p>");
  } else {
    server.send(400, "text/html", "<h1>Missing Mode Parameter</h1><p><a href='/'>Return</a></p>");
  }
}