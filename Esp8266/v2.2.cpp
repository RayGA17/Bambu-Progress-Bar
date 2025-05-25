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
#include <Ticker.h>

// LED 灯带配置
#define LED_PIN D4
#define LED_COUNT 20
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// MQTT 服务器配置
const char* mqttServer = "cn.mqtt.bambulab.com";
const int mqttPort = 8883;

// 全局对象
WiFiManager wm;
ESP8266WebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
Ticker watchdogTicker;

// 配置变量
char uid[32] = "";
char accessToken[256] = "";
char deviceID[32] = "";
int globalBrightness = 50;
char standbyMode[10] = "marquee";
bool overlayMarquee = false;
uint32_t progressBarColor = strip.Color(0, 255, 0); // 绿色
uint32_t standbyBreathingColor = strip.Color(0, 0, 255); // 蓝色
float progressBarBrightnessRatio = 1.0;
float standbyBrightnessRatio = 1.0;
unsigned long customPushallInterval = 30; // 自定义 pushall 间隔（秒）
String mqttTopicSub = "device/{DEVICE_ID}/report";
String mqttTopicPub = "device/{DEVICE_ID}/request";

// 时间控制
unsigned long lastReconnectAttempt = 0;
const long reconnectInterval = 5000;
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 10000;
unsigned long lastPushallTime = 0;
unsigned long lastOperationCheck = 0;
const long operationCheckInterval = 5000; // 5秒检查一次关键操作

// LED 动画控制
unsigned long lastLedUpdate = 0;
const long ledUpdateInterval = 33;
int marqueePosition = 0;
bool apClientConnected = false;

// LED 测试状态机
bool testingLed = false;
int testLedIndex = 0;
unsigned long lastTestLedUpdate = 0;
const long testLedInterval = 50;

// 单设备 Web 访问控制
IPAddress activeClientIP(0, 0, 0, 0);
unsigned long activeClientTimeout = 0;
const long clientTimeoutInterval = 60000;

// 状态枚举
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
State lastState = AP_MODE; // 用于模式判断监控
int printPercent = 0;
String gcodeState = "";
String mqttError = "";
int remainingTime = 0;
int layerNum = 0;

// 强制模式控制
enum ForcedMode {
  NONE,
  PROGRESS,
  STANDBY
};
ForcedMode forcedMode = NONE; // 跟踪是否强制模式

// 全局打印机状态存储
StaticJsonDocument<1024> printerState;

// 看门狗变量
volatile bool watchdogTriggered = false;
unsigned long lastWatchdogFeed = 0;
const long watchdogTimeout = 15000; // 15秒，降低灵敏度
unsigned long lastLedProcessTime = 0;
unsigned long lastModeJudgmentTime = 0;

// 函数声明
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
void watchdogCallback();

// 看门狗中断处理
// 仅监控 LED 渲染、模式判断和堆内存 < 300 字节
void watchdogCallback() {
  unsigned long currentMillis = millis();
  
  // 检查 LED 渲染进程
  if (lastLedProcessTime > 0 && currentMillis - lastLedProcessTime > watchdogTimeout) {
    Serial.println("看门狗：LED 渲染进程卡死！");
    watchdogTriggered = true;
  }
  
  // 检查模式判断进程
  if (lastModeJudgmentTime > 0 && currentMillis - lastModeJudgmentTime > watchdogTimeout) {
    Serial.println("看门狗：模式判断进程卡死！");
    watchdogTriggered = true;
  }
  
  // 检查堆内存
  if (ESP.getFreeHeap() < 300) {
    Serial.println("看门狗：堆内存低于 300 字节（剩余 " + String(ESP.getFreeHeap()) + "）！");
    watchdogTriggered = true;
  }
  
  if (watchdogTriggered) {
    Serial.println("看门狗触发！系统将重启。");
    ESP.restart();
  }
}

bool isConfigValid() {
  return strlen(uid) > 0 && strlen(accessToken) > 0 && strlen(deviceID) > 0;
}

bool isPrinting() {
  return (gcodeState == "RUNNING") || (remainingTime > 0) || (printPercent > 0) || (layerNum > 0);
}

void setup() {
  Serial.begin(115200);

  // 初始化 LED 灯带
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
    Serial.println("LED 灯带初始化失败");
  } else {
    Serial.println("LED 灯带初始化成功");
  }

  // 初始化 LittleFS
  if (!LittleFS.begin()) {
    Serial.println("无法挂载 LittleFS");
    return;
  }

  // 加载配置
  loadConfig();

  // 配置 WiFiManager
  WiFiManagerParameter custom_uid("uid", "用户ID", uid, 32);
  WiFiManagerParameter custom_access_token("accessToken", "访问令牌", accessToken, 256);
  WiFiManagerParameter custom_device_id("deviceID", "设备序列号", deviceID, 32);
  wm.addParameter(&custom_uid);
  wm.addParameter(&custom_access_token);
  wm.addParameter(&custom_device_id);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);

  // 尝试 WiFi 连接
  currentState = CONNECTING_WIFI;
  lastModeJudgmentTime = millis();
  if (!wm.autoConnect("BambuAP")) {
    Serial.println("WiFi 连接失败，进入 AP 模式");
    currentState = AP_MODE;
    lastModeJudgmentTime = millis();
  } else {
    Serial.println("WiFi 连接成功");
    currentState = CONNECTED_WIFI;
    lastModeJudgmentTime = millis();
    strncpy(uid, custom_uid.getValue(), sizeof(uid) - 1);
    uid[sizeof(uid) - 1] = '\0';
    strncpy(accessToken, custom_access_token.getValue(), sizeof(accessToken) - 1);
    accessToken[sizeof(accessToken) - 1] = '\0';
    strncpy(deviceID, custom_device_id.getValue(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    saveConfig();

    // 与 NTP 同步时间
    configTime(0, 0, "pool.ntp.org");
    time_t now = time(nullptr);
    unsigned long start = millis();
    while (now < 1000000000 && millis() - start < 5000) {
      delay(500);
      now = time(nullptr);
    }
    if (now >= 1000000000) {
      Serial.println("时间同步成功");
    } else {
      Serial.println("NTP 同步失败，继续使用本地时间");
    }
  }

  // 配置 MQTT
  if (isConfigValid()) {
    espClient.setInsecure();
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
  } else {
    Serial.println("配置为空，跳过 MQTT 初始化");
    mqttError = "配置为空，请设置用户ID、访问令牌和设备序列号";
  }

  // 启动 Web 服务器
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/testLed", handleTestLed);
  server.on("/log", handleLog);
  server.on("/clearCache", handleClearCache);
  server.on("/resetConfig", handleResetConfig);
  server.on("/switchMode", handleSwitchMode);
  server.onNotFound([]() { server.send(404, "text/plain", "页面不存在"); });
  server.begin();
  Serial.println("HTTP 服务器启动");

  // 初始化看门狗
  watchdogTicker.attach_ms(1000, watchdogCallback);
  lastWatchdogFeed = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // 监控关键进程
  if (currentMillis - lastOperationCheck > operationCheckInterval) {
    lastOperationCheck = currentMillis;
    if (WiFi.status() == WL_CONNECTED || currentState == AP_MODE) {
      lastWatchdogFeed = currentMillis;
    }
  }

  server.handleClient();

  // 检查 WiFi 状态
  if (currentState != AP_MODE && currentMillis - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi 断开，尝试重新连接");
      currentState = CONNECTING_WIFI;
      lastModeJudgmentTime = millis();
      wm.startConfigPortal("BambuAP");
      if (WiFi.status() == WL_CONNECTED) {
        currentState = CONNECTED_WIFI;
        lastModeJudgmentTime = millis();
        configTime(0, 0, "pool.ntp.org");
        time_t now = time(nullptr);
        unsigned long start = millis();
        while (now < 1000000000 && millis() - start < 5000) {
          delay(500);
          now = time(nullptr);
        }
        if (now >= 1000000000) {
          Serial.println("时间同步成功");
        } else {
          Serial.println("NTP 同步失败，继续使用本地时间");
        }
      } else {
        currentState = AP_MODE;
        lastModeJudgmentTime = millis();
      }
    }
  }

  // 处理 MQTT 连接
  if (currentState != AP_MODE && WiFi.status() == WL_CONNECTED && isConfigValid()) {
    if (!mqttClient.connected()) {
      currentState = CONNECTING_PRINTER;
      lastModeJudgmentTime = millis();
      reconnectMQTT();
    } else {
      mqttClient.loop();
    }
  }

  // 定期发送 pushall
  if (currentState == CONNECTED_PRINTER && currentMillis - lastPushallTime > customPushallInterval * 1000) {
    sendPushall();
    lastPushallTime = currentMillis;
  }

  // 处理 Web 客户端超时
  if (activeClientIP != IPAddress(0, 0, 0, 0) && currentMillis - activeClientTimeout > clientTimeoutInterval) {
    activeClientIP = IPAddress(0, 0, 0, 0);
    Serial.println("Web 客户端超时，释放锁定");
  }

  // 更新 LED
  updateLED();
  updateTestLed();

  // 监控堆内存
  if (ESP.getFreeHeap() < 8000) {
    Serial.println("警告：堆内存低，仅剩 " + String(ESP.getFreeHeap()) + " 字节");
  }

  // 如果状态变化，更新模式判断时间戳
  if (currentState != lastState) {
    lastModeJudgmentTime = millis();
    lastState = currentState;
    Serial.println("模式变更为：" + String(currentState));
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  currentState = AP_MODE;
  lastModeJudgmentTime = millis();
  apClientConnected = false;
  Serial.println("进入 AP 模式");
}

void saveConfigCallback() {
  saveConfig();
}

void loadConfig() {
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("未找到配置文件，使用默认值");
    return;
  }
  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("配置文件过大");
    configFile.close();
    return;
  }
  char buf[1024];
  configFile.readBytes(buf, size);
  configFile.close();
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, buf);
  if (error) {
    Serial.println("解析配置文件失败：" + String(error.c_str()));
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
  customPushallInterval = doc["customPushallInterval"] | 30; // 加载自定义 pushall 间隔
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
  doc["customPushallInterval"] = customPushallInterval; // 保存自定义 pushall 间隔
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("无法写入配置文件");
    return;
  }
  serializeJson(doc, configFile);
  configFile.close();
  sendPushall();
}

void reconnectMQTT() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastReconnectAttempt < reconnectInterval) return;
  lastReconnectAttempt = currentMillis;

  String clientID = "ESP8266Client-" + String(random(0xffff), HEX);
  Serial.println("连接到 MQTT 服务器：" + String(mqttServer));
  if (mqttClient.connect(clientID.c_str(), uid, accessToken)) {
    String topicSub = mqttTopicSub;
    topicSub.replace("{DEVICE_ID}", deviceID);
    mqttClient.subscribe(topicSub.c_str());
    currentState = CONNECTED_PRINTER;
    lastModeJudgmentTime = millis();
    mqttError = "";
    Serial.println("MQTT 连接成功");
    sendPushall();
  } else {
    mqttError = "MQTT 连接失败，错误码=" + String(mqttClient.state());
    Serial.println(mqttError);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("JSON 解析失败：" + String(error.c_str()));
    return;
  }
  if (doc.containsKey("print")) {
    JsonObject printData = doc["print"];
    if (doc.containsKey("command") && doc["command"] == "pushall") {
      printerState.clear();
      for (JsonPair kv : printData) {
        printerState[kv.key()] = kv.value();
      }
      Serial.println("全量更新完成");
    } else {
      for (JsonPair kv : printData) {
        printerState[kv.key()] = kv.value();
      }
      Serial.println("增量更新完成");
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

    // 根据打印状态更新状态，除非强制模式
    if (forcedMode == NONE) {
      if (isPrinting()) {
        currentState = PRINTING;
      } else if (gcodeState == "FAILED") {
        currentState = ERROR;
      } else {
        currentState = CONNECTED_PRINTER;
      }
    } else if (forcedMode == PROGRESS) {
      currentState = PRINTING;
    } else if (forcedMode == STANDBY) {
      currentState = CONNECTED_PRINTER;
    }

    lastModeJudgmentTime = millis();
    Serial.print("打印状态：" + gcodeState + "，进度：" + String(printPercent) + "%");
  }
}

void sendPushall() {
  StaticJsonDocument<256> pushall_request;
  pushall_request["pushing"]["sequence_id"] = String(millis());
  pushall_request["pushing"]["command"] = "pushall";
  pushall_request["pushing"]["version"] = 1;
  pushall_request["pushing"]["push_target"] = 1;
  String payload;
  serializeJson(pushall_request, payload);
  String topicPub = mqttTopicPub;
  topicPub.replace("{DEVICE_ID}", deviceID);
  mqttClient.publish(topicPub.c_str(), payload.c_str());
  Serial.println("发送 pushall 请求");
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
  lastLedProcessTime = millis();
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
    if (currentState == PRINTING) {
      // 进度条模式：根据 printPercent 调整 LED 数量和亮度
      float pixels = printPercent * LED_COUNT / 100.0;
      int fullPixels = floor(pixels);
      float partialPixel = pixels - fullPixels;
      for (int i = 0; i < fullPixels; i++) {
        strip.setPixelColor(i, progressBarColor);
      }
      if (fullPixels < LED_COUNT && partialPixel > 0) {
        uint8_t r = (progressBarColor >> 16) & 0xFF;
        uint8_t g = (progressBarColor >> 8) & 0xFF;
        uint8_t b = progressBarColor & 0xFF;
        strip.setPixelColor(fullPixels, strip.Color(r * partialPixel, g * partialPixel, b * partialPixel));
      }
      strip.setBrightness(constrain(globalBrightness * progressBarBrightnessRatio, 0, 255));
      
      // 如果启用跑马灯，叠加颜色
      if (overlayMarquee) {
        for (int i = 0; i <= fullPixels; i++) {
          float pos = (float)(i + marqueePosition) / LED_COUNT;
          uint32_t rainbowColor = getRainbowColor(pos);
          uint8_t r = (rainbowColor >> 16) & 0xFF;
          uint8_t g = (rainbowColor >> 8) & 0xFF;
          uint8_t b = rainbowColor & 0xFF;
          uint8_t pr = (progressBarColor >> 16) & 0xFF;
          uint8_t pg = (progressBarColor >> 8) & 0xFF;
          uint8_t pb = progressBarColor & 0xFF;
          strip.setPixelColor(i, strip.Color((r + pr) / 2, (g + pg) / 2, (b + pb) / 2));
        }
        marqueePosition = (marqueePosition + 1) % LED_COUNT;
      }
    } else {
      // 待机模式
      if (strcmp(standbyMode, "marquee") == 0) {
        for (int i = 0; i < LED_COUNT; i++) {
          float pos = (float)(i + marqueePosition) / LED_COUNT;
          strip.setPixelColor(i, getRainbowColor(pos));
        }
        marqueePosition = (marqueePosition + 1) % LED_COUNT;
        strip.setBrightness(constrain(globalBrightness * standbyBrightnessRatio, 0, 255));
      } else if (strcmp(standbyMode, "breathing") == 0) {
        float brightness = (sin(millis() / 1000.0 * PI) + 1) / 2.0;
        int scaledBrightness = brightness * globalBrightness * standbyBrightnessRatio;
        for (int i = 0; i < LED_COUNT; i++) {
          strip.setPixelColor(i, standbyBreathingColor);
        }
        strip.setBrightness(constrain(scaledBrightness, 0, 255));
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
  lastLedProcessTime = millis();
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
    Serial.println("新客户端连接：" + clientIP.toString());
    return true;
  } else if (clientIP == activeClientIP) {
    activeClientTimeout = millis();
    return true;
  } else {
    Serial.println("拒绝客户端：" + clientIP.toString());
    return false;
  }
}

void handleRoot() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }

  String html;
  html.reserve(4096);
  html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "<title>打印机状态显示</title>"
           "<style>"
           "body { font-family: 'PingFang SC', Arial, sans-serif; margin: 20px; background-color: #f4f4f4; }"
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
           "    alert('用户ID、访问令牌和设备序列号不能为空！');"
           "    return false;"
           "  }"
           "  var brightness = document.forms['configForm']['brightness'].value;"
           "  if (brightness < 0 || brightness > 255) {"
           "    alert('全局亮度必须在 0 到 255 之间！');"
           "    return false;"
           "  }"
           "  var pbr = document.forms['configForm']['progressBarBrightnessRatio'].value;"
           "  var sbr = document.forms['configForm']['standbyBrightnessRatio'].value;"
           "  if (pbr < 0 || pbr > 1 || sbr < 0 || sbr > 1) {"
           "    alert('亮度比例必须在 0 到 1 之间！');"
           "    return false;"
           "  }"
           "  var pushallInterval = document.forms['configForm']['customPushallInterval'].value;"
           "  if (pushallInterval < 10 || pushallInterval > 600) {"
           "    alert('全量包请求间隔必须在 10 到 600 秒之间！');"
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
           "<h1>打印机状态显示</h1>"
           "<p>设备状态：");

  switch (currentState) {
    case AP_MODE: html += "AP 模式"; break;
    case CONNECTING_WIFI: html += "正在连接 WiFi"; break;
    case CONNECTED_WIFI: html += "WiFi 已连接"; break;
    case CONNECTING_PRINTER: html += "正在连接打印机"; break;
    case CONNECTED_PRINTER: html += "打印机已连接"; break;
    case PRINTING: html += "打印中（进度：" + String(printPercent) + "%）"; break;
    case ERROR: html += "打印错误"; break;
  }
  html += "</p>";

  html += "<form name='configForm' action='/config' method='POST' onsubmit='return validateForm()'>"
          "<label>用户ID：<input type='text' name='uid' value='" + String(uid) + "'></label>"
          "<label>访问令牌：<input type='text' name='accessToken' value='" + String(accessToken) + "' maxlength='256'></label>"
          "<label>设备序列号：<input type='text' name='deviceID' value='" + String(deviceID) + "'></label>"
          "<label>全局亮度（0-255）：<input type='number' name='brightness' min='0' max='255' value='" + String(globalBrightness) + "'></label>"
          "<label>待机模式：<select name='standbyMode'>"
          "<option value='marquee'" + (strcmp(standbyMode, "marquee") == 0 ? " selected" : "") + ">彩虹跑马灯</option>"
          "<option value='breathing'" + (strcmp(standbyMode, "breathing") == 0 ? " selected" : "") + ">呼吸灯</option>"
          "</select></label>"
          "<label>进度条颜色：<input type='color' name='progressBarColor' value='#" + String(progressBarColor, HEX).substring(2) + "'></label>"
          "<label>待机呼吸灯颜色：<input type='color' name='standbyBreathingColor' value='#" + String(standbyBreathingColor, HEX).substring(2) + "'></label>"
          "<label>进度条亮度比例（0.0-1.0）：<input type='number' step='0.1' min='0' max='1' name='progressBarBrightnessRatio' value='" + String(progressBarBrightnessRatio) + "'></label>"
          "<label>待机亮度比例（0.0-1.0）：<input type='number' step='0.1' min='0' max='1' name='standbyBrightnessRatio' value='" + String(standbyBrightnessRatio) + "'></label>"
          "<label>全量包请求间隔（10-600秒）：<input type='number' name='customPushallInterval' min='10' max='600' value='" + String(customPushallInterval) + "'></label>"
          "<label><input type='checkbox' name='overlayMarquee'" + (overlayMarquee ? " checked" : "") + "> 在进度条上叠加跑马灯</label>"
          "<button type='submit'>保存配置</button>"
          "</form>";

  html += "<form action='/testLed' method='POST'><button type='submit'>测试 LED 灯带</button></form>"
          "<form action='/clearCache' method='POST'><button type='submit'>清除缓存</button></form>"
          "<form action='/resetConfig' method='POST'><button type='submit'>重置配置</button></form>"
          "<form action='/switchMode' method='POST'>"
          "<label>切换到：<select name='mode'>"
          "<option value='progress'>进度条</option>"
          "<option value='standby'>待机</option>"
          "</select></label><button type='submit'>切换模式</button></form>"
          "<div class='log' id='log'>正在加载日志...</div></div>"
          "<script>updateLog();</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleConfig() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
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
    String colorStr = server.arg("progressBarColor").substring(1);
    progressBarColor = colorStr.length() == 6 ? strtoul(colorStr.c_str(), NULL, 16) : progressBarColor;
    colorStr = server.arg("standbyBreathingColor").substring(1);
    standbyBreathingColor = colorStr.length() == 6 ? strtoul(colorStr.c_str(), NULL, 16) : standbyBreathingColor;
    progressBarBrightnessRatio = server.arg("progressBarBrightnessRatio").toFloat();
    standbyBrightnessRatio = server.arg("standbyBrightnessRatio").toFloat();
    customPushallInterval = server.arg("customPushallInterval").toInt(); // 更新 pushall 间隔
    strip.setBrightness(constrain(globalBrightness, 0, 255));
    saveConfig();
    if (isConfigValid()) {
      mqttClient.setServer(mqttServer, mqttPort);
      currentState = CONNECTING_PRINTER;
      lastModeJudgmentTime = millis();
    }
    activeClientIP = IPAddress(0, 0, 0, 0);
    // 发送 JavaScript 弹窗响应，保持页面不刷新
    String response = "<script>alert('配置保存成功！'); window.location.href='/';</script>";
    server.send(200, "text/html; charset=utf-8", response);
  } else {
    // 发送错误弹窗
    String response = "<script>alert('缺少必要参数！'); window.location.href='/';</script>";
    server.send(400, "text/html; charset=utf-8", response);
  }
}

void handleTestLed() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  testingLed = true;
  testLedIndex = 0;
  lastTestLedUpdate = millis();
  String response = "<script>alert('开始测试 LED 灯带！'); window.location.href='/';</script>";
  server.send(200, "text/html; charset=utf-8", response);
}

void handleLog() {
  String ledMode;
  if (currentState == AP_MODE) {
    ledMode = apClientConnected ? "绿色常亮" : "黄绿交替";
  } else if (currentState == CONNECTING_WIFI || currentState == CONNECTING_PRINTER) {
    ledMode = "红蓝交替";
  } else if (currentState == CONNECTED_WIFI) {
    ledMode = "蓝色常亮，红色闪烁";
  } else if (currentState == CONNECTED_PRINTER || currentState == PRINTING) {
    if (currentState == PRINTING) {
      ledMode = "进度条（" + String(overlayMarquee ? "带彩虹跑马灯" : "纯色") + "）";
    } else {
      ledMode = String(standbyMode) == "marquee" ? "彩虹跑马灯" : "呼吸灯";
    }
  } else if (currentState == ERROR) {
    ledMode = "红色闪烁";
  }

  String logContent = "WiFi 状态：" + String(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接") + "<br>"
                      "配置状态：" + String(isConfigValid() ? "有效" : "无效") + "<br>"
                      "MQTT 状态：" + String(mqttClient.connected() ? "已连接" : mqttError) + "<br>"
                      "打印机状态：" + gcodeState + "<br>"
                      "打印进度：" + String(printPercent) + "%<br>"
                      "剩余时间：" + String(remainingTime) + " 分钟<br>"
                      "层数：" + String(layerNum) + "<br>"
                      "喷嘴温度：" + String(printerState["nozzle_temper"].as<float>()) + "°C<br>"
                      "热床温度：" + String(printerState["bed_temper"].as<float>()) + "°C<br>"
                      "LED 模式：" + ledMode + "<br>"
                      "进度条颜色：#" + String(progressBarColor, HEX).substring(2) + "<br>"
                      "待机呼吸灯颜色：#" + String(standbyBreathingColor, HEX).substring(2) + "<br>"
                      "进度条亮度比例：" + String(progressBarBrightnessRatio) + "<br>"
                      "待机亮度比例：" + String(standbyBrightnessRatio) + "<br>"
                      "叠加跑马灯：" + String(overlayMarquee ? "启用" : "禁用") + "<br>"
                      "强制模式：" + String(forcedMode == NONE ? "无" : (forcedMode == PROGRESS ? "进度条" : "待机")) + "<br>"
                      "全量包间隔：" + String(customPushallInterval) + " 秒<br>"
                      "可用堆内存：" + String(ESP.getFreeHeap()) + " 字节";
  server.send(200, "text/html; charset=utf-8", logContent);
}

void handleClearCache() {
  String response = "<script>alert('缓存已清除！'); window.location.href='/';</script>";
  server.send(200, "text/html; charset=utf-8", response);
}

void handleResetConfig() {
  LittleFS.remove("/config.json");
  forcedMode = NONE;
  String response = "<script>alert('配置已重置，设备将重启！'); window.location.href='/';</script>";
  server.send(200, "text/html; charset=utf-8", response);
  delay(1000);
  ESP.restart();
}

void handleSwitchMode() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    String modeName = mode == "progress" ? "进度条" : "待机";
    if (mode == "progress") {
      forcedMode = PROGRESS;
      currentState = PRINTING;
    } else if (mode == "standby") {
      forcedMode = STANDBY;
      currentState = CONNECTED_PRINTER;
    }
    lastModeJudgmentTime = millis();
    String response = "<script>alert('模式已切换到 " + modeName + "！'); window.location.href='/';</script>";
    server.send(200, "text/html; charset=utf-8", response);
  } else {
    String response = "<script>alert('缺少模式参数！'); window.location.href='/';</script>";
    server.send(400, "text/html; charset=utf-8", response);
  }
}