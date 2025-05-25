#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <WiFiClientSecure.h>
#include <pgmspace.h>

// 灯带配置
#define LED_PIN D4
#define LED_COUNT 20
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// 全局对象
WiFiManager wm;
ESP8266WebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// 配置变量
char printerIP[16] = ""; // 固定长度 char 数组
char pairingCode[20] = "";
char deviceID[32] = "";
int globalBrightness = 50; // 默认亮度降低到 50
char standbyMode[10] = "marquee"; // 跑马灯或呼吸灯
bool overlayMarquee = false; // 进度条叠加跑马灯
char mqttServer[16] = "";
String mqttPassword = "";
String mqttTopic = "device/{DEVICE_ID}/report";

// 重连控制
unsigned long lastReconnectAttempt = 0;
const long reconnectInterval = 5000;
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 10000; // 每 10 秒检查 WiFi
int mqttRetryCount = 0;
const int maxMqttRetries = 10; // 最大重试次数

// 灯带动画控制
unsigned long lastLedUpdate = 0;
const long ledUpdateInterval = 33; // 33ms (约 30Hz)
int marqueePosition = 0;
bool apClientConnected = false;

// 测试灯带状态机
bool testingLed = false;
int testLedIndex = 0;
unsigned long lastTestLedUpdate = 0;
const long testLedInterval = 50;

// 单设备网页访问控制
IPAddress activeClientIP(0, 0, 0, 0);
unsigned long activeClientTimeout = 0;
const long clientTimeoutInterval = 60000; // 60 秒超时

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
int printPercent = 0;
String gcodeState = "";
String mqttError = "";

// CA 证书（拓竹 P1S）
const char caCert[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDZTCCAk2gAwIBAgIUV1FckwXElyek1onFnQ9kL7Bk4N8wDQYJKoZIhvcNAQEL
BQAwQjELMAkGA1UEBhMCQ04xIjAgBgNVBAoMGUJCTCBUZWNobm9sb2dpZXMgQ28u
LCBMdGQxDzANBgNVBAMMBkJCTCBDQTAeFw0yMjA0MDQwMzQyMTFaFw0zMjA0MDEw
MzQyMTFaMEIxCzAJBgNVBAYTAkNOMSIwIAYDVQQKDBlCQkwgVGVjaG5vbG9naWVz
IENvLiwgTHRkMQ8wDQYDVQQDDAZCQkwgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IB
DwAwggEKAoIBAQDL3pnDdxGOk5Z6vugiT4dpM0ju+3Xatxz09UY7mbj4tkIdby4H
oeEdiYSZjc5LJngJuCHwtEbBJt1BriRdSVrF6M9D2UaBDyamEo0dxwSaVxZiDVWC
eeCPdELpFZdEhSNTaT4O7zgvcnFsfHMa/0vMAkvE7i0qp3mjEzYLfz60axcDoJLk
p7n6xKXI+cJbA4IlToFjpSldPmC+ynOo7YAOsXt7AYKY6Glz0BwUVzSJxU+/+VFy
/QrmYGNwlrQtdREHeRi0SNK32x1+bOndfJP0sojuIrDjKsdCLye5CSZIvqnbowwW
1jRwZgTBR29Zp2nzCoxJYcU9TSQp/4KZuWNVAgMBAAGjUzBRMB0GA1UdDgQWBBSP
NEJo3GdOj8QinsV8SeWr3US+HjAfBgNVHSMEGDAWgBSPNEJo3GdOj8QinsV8SeWr
3US+HjAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQABlBIT5ZeG
fgcK1LOh1CN9sTzxMCLbtTPFF1NGGA13mApu6j1h5YELbSKcUqfXzMnVeAb06Htu
3CoCoe+wj7LONTFO++vBm2/if6Jt/DUw1CAEcNyqeh6ES0NX8LJRVSe0qdTxPJuA
BdOoo96iX89rRPoxeed1cpq5hZwbeka3+CJGV76itWp35Up5rmmUqrlyQOr/Wax6
itosIzG0MfhgUzU51A2P/hSnD3NDMXv+wUY/AvqgIL7u7fbDKnku1GzEKIkfH8hm
Rs6d8SCU89xyrwzQ0PR853irHas3WrHVqab3P+qNwR0YirL0Qk7Xt/q3O1griNg2
Blbjg3obpHo9
-----END CERTIFICATE-----
)EOF";

void setup() {
  Serial.begin(115200);

  // 初始化灯带并检查
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
    Serial.println("灯带初始化失败，请检查连接");
  } else {
    Serial.println("灯带初始化成功");
  }

  // 初始化 SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount SPIFFS");
    return;
  }

  // 加载配置
  loadConfig();

  // 配置 WiFiManager
  WiFiManagerParameter custom_printer_ip("printerIP", "打印机 IP", printerIP, 16);
  WiFiManagerParameter custom_pairing_code("pairingCode", "配对码", pairingCode, 20);
  WiFiManagerParameter custom_device_id("deviceID", "设备序列号", deviceID, 32);
  wm.addParameter(&custom_printer_ip);
  wm.addParameter(&custom_pairing_code);
  wm.addParameter(&custom_device_id);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);

  // 尝试连接 WiFi
  currentState = CONNECTING_WIFI;
  if (!wm.autoConnect("BambuAP")) {
    Serial.println("WiFi 连接失败，进入 AP 模式");
    currentState = AP_MODE;
  } else {
    Serial.println("WiFi 连接成功");
    currentState = CONNECTED_WIFI;
    strncpy(printerIP, custom_printer_ip.getValue(), sizeof(printerIP) - 1);
    printerIP[sizeof(printerIP) - 1] = '\0';
    strncpy(pairingCode, custom_pairing_code.getValue(), sizeof(pairingCode) - 1);
    pairingCode[sizeof(pairingCode) - 1] = '\0';
    strncpy(deviceID, custom_device_id.getValue(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    strncpy(mqttServer, printerIP, sizeof(mqttServer) - 1);
    mqttServer[sizeof(mqttServer) - 1] = '\0';
    mqttPassword = pairingCode;
    saveConfig();
  }

  // 配置 MQTT
  espClient.setCACert(caCert);
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);

  // 启动网页服务器
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/testLed", handleTestLed);
  server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });
  server.begin();
  Serial.println("HTTP 服务器已启动");
}

void loop() {
  // 处理 WiFiManager、网页服务器和 MQTT
  wm.process();
  server.handleClient();

  // 检查 WiFi 状态
  if (currentState != AP_MODE && millis() - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi 断开，尝试重连");
      currentState = CONNECTING_WIFI;
      wm.startConfigPortal("BambuAP");
      if (WiFi.status() == WL_CONNECTED) {
        currentState = CONNECTED_WIFI;
        strncpy(mqttServer, printerIP, sizeof(mqttServer) - 1);
        mqttServer[sizeof(mqttServer) - 1] = '\0';
        mqttPassword = pairingCode;
        mqttRetryCount = 0; // 重置 MQTT 重试计数
      } else {
        currentState = AP_MODE;
      }
    }
  }

  // 处理 MQTT
  if (currentState != AP_MODE && WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected() && currentState != CONNECTING_WIFI) {
      currentState = CONNECTING_PRINTER;
      reconnectMQTT();
    } else {
      mqttClient.loop();
    }
  }

  // 检查网页客户端超时
  if (activeClientIP != IPAddress(0, 0, 0, 0) && millis() - activeClientTimeout > clientTimeoutInterval) {
    activeClientIP = IPAddress(0, 0, 0, 0);
    Serial.println("网页客户端超时，已释放锁");
  }

  // 更新灯带
  updateLED();

  // 非阻塞灯带测试
  updateTestLed();

  // 低内存警告
  if (ESP.getFreeHeap() < 10000) {
    Serial.println("警告：可用堆内存低，仅剩 " + String(ESP.getFreeHeap()) + " 字节");
  }
}

// 配置模式回调
void configModeCallback(WiFiManager *myWiFiManager) {
  currentState = AP_MODE;
  apClientConnected = false;
  Serial.println("进入 AP 模式");
}

// 保存配置回调
void saveConfigCallback() {
  saveConfig();
}

// 加载配置
void loadConfig() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("未找到配置文件，使用默认值");
    return;
  }
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, buf.get());
  if (error) {
    Serial.println("解析配置文件失败");
    return;
  }
  strncpy(printerIP, doc["printerIP"] | "", sizeof(printerIP) - 1);
  printerIP[sizeof(printerIP) - 1] = '\0';
  strncpy(pairingCode, doc["pairingCode"] | "", sizeof(pairingCode) - 1);
  pairingCode[sizeof(pairingCode) - 1] = '\0';
  strncpy(deviceID, doc["deviceID"] | "", sizeof(deviceID) - 1);
  deviceID[sizeof(deviceID) - 1] = '\0';
  globalBrightness = doc["brightness"] | 50;
  strncpy(standbyMode, doc["standbyMode"] | "marquee", sizeof(standbyMode) - 1);
  standbyMode[sizeof(standbyMode) - 1] = '\0';
  overlayMarquee = doc["overlayMarquee"] | false;
  strncpy(mqttServer, printerIP, sizeof(mqttServer) - 1);
  mqttServer[sizeof(mqttServer) - 1] = '\0';
  mqttPassword = pairingCode;
  strip.setBrightness(constrain(globalBrightness, 0, 255));
}

// 保存配置
void saveConfig() {
  StaticJsonDocument<512> doc;
  doc["printerIP"] = printerIP;
  doc["pairingCode"] = pairingCode;
  doc["deviceID"] = deviceID;
  doc["brightness"] = globalBrightness;
  doc["standbyMode"] = standbyMode;
  doc["overlayMarquee"] = overlayMarquee;
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("无法写入配置文件");
    return;
  }
  serializeJson(doc, configFile);
  configFile.close();
}

// MQTT 重连
void reconnectMQTT() {
  if (millis() - lastReconnectAttempt < reconnectInterval) return;
  lastReconnectAttempt = millis();
  if (mqttRetryCount >= maxMqttRetries) {
    mqttError = "MQTT 重试次数超限，请检查打印机 IP 和配对码";
    Serial.println(mqttError);
    return;
  }
  mqttClient.disconnect(); // 清除旧连接
  String clientID = "ESP8266Client-" + String(random(0xffff), HEX);
  if (mqttClient.connect(clientID.c_str(), "bblp", mqttPassword.c_str())) {
    String topic = mqttTopic;
    topic.replace("{DEVICE_ID}", deviceID);
    mqttClient.subscribe(topic.c_str());
    currentState = CONNECTED_PRINTER;
    mqttError = "";
    mqttRetryCount = 0;
    Serial.println("MQTT 连接成功");
    String pushAll = "{\"pushing\": {\"sequence_id\": \"0\", \"command\": \"pushall\", \"version\": 1, \"push_target\": 1}}";
    mqttClient.publish(("device/" + deviceID + "/request").c_str(), pushAll.c_str());
  } else {
    mqttRetryCount++;
    mqttError = "MQTT 连接失败，错误码=" + String(mqttClient.state()) + "，重试 " + String(mqttRetryCount) + "/" + String(maxMqttRetries);
    Serial.println(mqttError);
  }
}

// MQTT 回调
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("JSON 解析失败: ");
    Serial.println(error.c_str());
    return;
  }
  if (doc.containsKey("print")) {
    gcodeState = doc["print"]["gcode_state"].as<String>();
    int rawPercent = doc["print"]["print_percent"].as<int>();
    printPercent = constrain(rawPercent, 0, 100); // 边界检查
    if (gcodeState == "RUNNING") {
      currentState = PRINTING;
    } else if (gcodeState == "FAILED") {
      currentState = ERROR;
    } else if (gcodeState == "IDLE" || gcodeState == "FINISH") {
      currentState = CONNECTED_PRINTER;
    }
    Serial.print("打印状态: ");
    Serial.print(gcodeState);
    Serial.print(", 进度: ");
    Serial.println(printPercent);
  }
}

// 获取彩色跑马灯颜色
uint32_t getMarqueeColor(int index) {
  int colorIndex = index % 3; // 逐灯循环颜色
  switch (colorIndex) {
    case 0: return strip.Color(255, 0, 0); // 红色
    case 1: return strip.Color(0, 255, 0); // 绿色
    case 2: return strip.Color(0, 0, 255); // 蓝色
  }
  return strip.Color(0, 0, 0);
}

// 更新灯带
void updateLED() {
  static unsigned long lastUpdate = 0;
  static unsigned long connectTime = 0;
  if (millis() - lastUpdate < ledUpdateInterval) return;
  lastUpdate = millis();
  if (testingLed) return; // 测试模式优先
  strip.clear();

  // 检测 AP 客户端连接
  if (currentState == AP_MODE) {
    apClientConnected = WiFi.softAPgetStationNum() > 0;
  }

  switch (currentState) {
    case AP_MODE:
      if (apClientConnected) {
        strip.setPixelColor(0, strip.Color(0, 255, 0)); // 绿色常亮
      } else {
        strip.setPixelColor(0, (millis() / 500) % 2 ? strip.Color(255, 255, 0) : strip.Color(0, 255, 0)); // 黄绿交替
      }
      break;
    case CONNECTING_WIFI:
      strip.setPixelColor(0, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : strip.Color(0, 0, 255)); // 红蓝交替
      break;
    case CONNECTED_WIFI:
      strip.setPixelColor(0, strip.Color(0, 0, 255)); // 蓝色常亮
      strip.setPixelColor(1, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : 0); // 红色闪烁
      break;
    case CONNECTING_PRINTER:
      strip.setPixelColor(0, strip.Color(0, 0, 255)); // 蓝色常亮
      strip.setPixelColor(1, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : 0); // 红色闪烁
      break;
    case CONNECTED_PRINTER:
      if (connectTime == 0 && !gcodeState.isEmpty()) {
        connectTime = millis();
      }
      if (connectTime && millis() - connectTime < 2000) {
        strip.setPixelColor(1, (millis() / 500) % 2 ? strip.Color(255, 255, 0) : 0); // 黄色闪两次
      } else {
        connectTime = 0;
      }
      if (gcodeState == "IDLE" || gcodeState == "FINISH") {
        if (strcmp(standbyMode, "marquee") == 0) {
          for (int i = 0; i < 5; i++) {
            int pos = (marqueePosition + i) % LED_COUNT;
            strip.setPixelColor(pos, getMarqueeColor(i)); // 彩色跑马灯
          }
          marqueePosition = (marqueePosition + 1) % LED_COUNT;
        } else if (strcmp(standbyMode, "breathing") == 0) {
          float brightness = (sin(millis() / 1000.0 * PI) + 1) / 2.0; // 2秒周期
          int scaledBrightness = brightness * globalBrightness;
          for (int i = 0; i < LED_COUNT; i++) {
            strip.setPixelColor(i, strip.Color(0, 0, scaledBrightness)); // 蓝色呼吸
          }
        }
      }
      break;
    case PRINTING:
      float pixels = printPercent * LED_COUNT / 100.0;
      int fullPixels = floor(pixels);
      float partialPixel = pixels - fullPixels;
      for (int i = 0; i < fullPixels; i++) {
        strip.setPixelColor(i, strip.Color(0, 255, 0)); // 全亮绿色
      }
      if (fullPixels < LED_COUNT) {
        strip.setPixelColor(fullPixels, strip.Color(0, 255 * partialPixel, 0)); // 次像素渲染
      }
      if (overlayMarquee) {
        for (int i = 0; i < 5; i++) {
          int pos = (marqueePosition + i) % LED_COUNT;
          strip.setPixelColor(pos, getMarqueeColor(i)); // 叠加彩色跑马灯
        }
        marqueePosition = (marqueePosition + 1) % LED_COUNT;
      }
      break;
    case ERROR:
      for (int i = 0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, (millis() / 500) % 2 ? strip.Color(255, 0, 0) : 0); // 红色闪烁
      }
      break;
  }

  strip.show();
}

// 非阻塞灯带测试
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

// 检查网页访问权限
bool checkClientAccess() {
  IPAddress clientIP = server.client().remoteIP();
  if (activeClientIP == IPAddress(0, 0, 0, 0)) {
    activeClientIP = clientIP;
    activeClientTimeout = millis();
    Serial.println("新客户端接入: " + clientIP.toString());
    return true;
  } else if (clientIP == activeClientIP) {
    activeClientTimeout = millis(); // 刷新超时
    return true;
  } else {
    Serial.println("拒绝客户端 " + clientIP.toString() + "，已有客户端 " + activeClientIP.toString());
    return false;
  }
}

// 网页根页面
void handleRoot() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一设备正在配置，请稍后重试。</p>");
    return;
  }

  String html;
  html.reserve(4096); // 预分配 4KB 缓冲区
  html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "<meta http-equiv='refresh' content='5'>"
           "<title>拓竹打印机状态显示</title>"
           "<style>"
           "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f4; }"
           "h1 { color: #333; }"
           ".container { max-width: 600px; margin: auto; background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
           "label { display: block; margin: 10px 0 5px; color: #555; }"
           "input[type='text'], input[type='number'], select, input[type='checkbox'] { width: 100%; padding: 8px; margin-bottom: 10px; border: 1px solid #ccc; border-radius: 4px; }"
           "button { background-color: #007bff; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; }"
           "button:hover { background-color: #0056b3; }"
           ".log { margin-top: 20px; padding: 10px; background: #e9ecef; border-radius: 4px; white-space: pre-wrap; }"
           "</style>"
           "<script>"
           "function validateForm() {"
           "  var ip = document.forms['configForm']['printerIP'].value;"
           "  var brightness = document.forms['configForm']['brightness'].value;"
           "  var ipRegex = /^(\d{1,3}\.){3}\d{1,3}$/;"
           "  if (!ipRegex.test(ip)) {"
           "    alert('请输入有效的 IP 地址！');"
           "    return false;"
           "  }"
           "  if (brightness < 0 || brightness > 255) {"
           "    alert('亮度必须在 0-255 之间！');"
           "    return false;"
           "  }"
           "  return true;"
           "}"
           "</script></head><body>"
           "<div class='container'>"
           "<h1>拓竹打印机状态显示</h1>"
           "<p>设备状态: ");

  switch (currentState) {
    case AP_MODE: html += "AP 模式"; break;
    case CONNECTING_WIFI: html += "连接 WiFi 中"; break;
    case CONNECTED_WIFI: html += "WiFi 已连接"; break;
    case CONNECTING_PRINTER: html += "连接打印机中"; break;
    case CONNECTED_PRINTER: html += "已连接打印机"; break;
    case PRINTING: html += "打印中（进度: " + String(printPercent) + "%）"; break;
    case ERROR: html += "打印错误"; break;
  }
  html += "</p>";

  html += "<form name='configForm' action='/config' method='POST' onsubmit='return validateForm()'>"
          "<label>打印机 IP: <input type='text' name='printerIP' value='" + String(printerIP) + "'></label>"
          "<label>配对码: <input type='text' name='pairingCode' value='" + String(pairingCode) + "'></label>"
          "<label>设备序列号: <input type='text' name='deviceID' value='" + String(deviceID) + "'></label>"
          "<label>全局亮度 (0-255): <input type='number' name='brightness' min='0' max='255' value='" + String(globalBrightness) + "'></label>"
          "<label>待机模式: <select name='standbyMode'>"
          "<option value='marquee'" + (strcmp(standbyMode, "marquee") == 0 ? " selected" : "") + ">彩色跑马灯</option>"
          "<option value='breathing'" + (strcmp(standbyMode, "breathing") == 0 ? " selected" : "") + ">蓝色呼吸灯</option>"
          "</select></label>"
          "<label><input type='checkbox' name='overlayMarquee'" + (overlayMarquee ? " checked" : "") + "> 进度条叠加彩色跑马灯</label>"
          "<button type='submit'>保存配置</button>"
          "</form>";

  html += "<form action='/testLed' method='POST'>"
          "<button type='submit'>测试灯带</button>"
          "</form>";

  html += "<div class='log'>实时日志:<br>"
          "WiFi 状态: " + String(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接") + "<br>"
          "MQTT 状态: " + String(mqttClient.connected() ? "已连接" : mqttError) + "<br>"
          "打印机状态: " + gcodeState + "<br>"
          "打印进度: " + String(printPercent) + "%<br>"
          "灯带模式: ";
  switch (currentState) {
    case AP_MODE: html += apClientConnected ? "绿色常亮" : "黄绿交替"; break;
    case CONNECTING_WIFI: html += "红蓝交替"; break;
    case CONNECTED_WIFI: html += "蓝色常亮，红色闪烁"; break;
    case CONNECTING_PRINTER: html += "蓝色常亮，红色闪烁"; break;
    case CONNECTED_PRINTER: html += gcodeState == "IDLE" ? (strcmp(standbyMode, "marquee") == 0 ? "彩色跑马灯" : "蓝色呼吸灯") : "黄色闪两次后熄灭"; break;
    case PRINTING: html += "绿色进度条" + String(overlayMarquee ? " + 彩色跑马灯" : ""); break;
    case ERROR: html += "红色闪烁"; break;
  }
  html += "<br>全局亮度: " + String(globalBrightness) + "<br>"
          "可用堆内存: " + String(ESP.getFreeHeap()) + " 字节</div></div></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// 处理配置提交
void handleConfig() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一设备正在配置，请稍后重试。</p>");
    return;
  }

  if (server.hasArg("printerIP") && server.hasArg("pairingCode") && server.hasArg("deviceID")) {
    strncpy(printerIP, server.arg("printerIP").c_str(), sizeof(printerIP) - 1);
    printerIP[sizeof(printerIP) - 1] = '\0';
    strncpy(pairingCode, server.arg("pairingCode").c_str(), sizeof(pairingCode) - 1);
    pairingCode[sizeof(pairingCode) - 1] = '\0';
    strncpy(deviceID, server.arg("deviceID").c_str(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    globalBrightness = server.arg("brightness").toInt();
    strncpy(standbyMode, server.arg("standbyMode").c_str(), sizeof(standbyMode) - 1);
    standbyMode[sizeof(standbyMode) - 1] = '\0';
    overlayMarquee = server.hasArg("overlayMarquee") && server.arg("overlayMarquee") == "on";
    strip.setBrightness(constrain(globalBrightness, 0, 255));
    strncpy(mqttServer, printerIP, sizeof(mqttServer) - 1);
    mqttServer[sizeof(mqttServer) - 1] = '\0';
    mqttPassword = pairingCode;
    saveConfig();
    mqttClient.setServer(mqttServer, mqttPort);
    currentState = CONNECTING_PRINTER;
    activeClientIP = IPAddress(0, 0, 0, 0); // 释放客户端锁
    server.send(200, "text/html", "<h1>配置已保存</h1><p><a href='/'>返回</a></p>");
  } else {
    server.send(400, "text/html", "<h1>缺少参数</h1><p><a href='/'>返回</a></p>");
  }
}

// 触发灯带测试
void handleTestLed() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一设备正在配置，请稍后重试。</p>");
    return;
  }
  testingLed = true;
  testLedIndex = 0;
  lastTestLedUpdate = millis();
  server.send(200, "text/html", "<h1>开始灯带测试</h1><p><a href='/'>返回</a></p>");
}