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
#include <pgmspace.h>

// LED 灯带配置
#define LED_PIN D4
#define LED_COUNT 20
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// MQTT 服务器配置（存储在 PROGMEM）
const char MQTT_SERVER[] PROGMEM = "cn.mqtt.bambulab.com";
const int MQTT_PORT = 8883;

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

// MQTT 主题（存储在 PROGMEM）
const char MQTT_TOPIC_SUB[] PROGMEM = "device/{DEVICE_ID}/report";
const char MQTT_TOPIC_PUB[] PROGMEM = "device/{DEVICE_ID}/request";

// 时间控制
unsigned long lastReconnectAttempt = 0;
unsigned long reconnectDelay = 1000; // 初始重连间隔 1 秒
const unsigned long maxReconnectDelay = 120000; // 最大重连间隔 120 秒
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 10000;
unsigned long lastPushallTime = 0;
unsigned long lastOperationCheck = 0;
const long operationCheckInterval = 5000;
unsigned long lastHeapWarningTime = 0;
unsigned long webResponseStartTime = 0;
unsigned long lastMqttResponseTime = 0;
const long mqttResponseTimeout = 60000; // 60秒 MQTT 响应超时
const long webResponseTimeout = 20000; // 20秒 Web 响应超时
bool isWebServing = false;
bool pendingPushall = false;
bool printerOffline = false;
bool pauseLedUpdate = false;
bool pauseMqttUpdate = false; // 新增：暂停 MQTT 处理

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
State lastState = AP_MODE;
int printPercent = 0;
String gcodeState = "";
int remainingTime = 0;
int layerNum = 0;

// 强制模式控制
enum ForcedMode {
  NONE,
  PROGRESS,
  STANDBY
};
ForcedMode forcedMode = NONE;

// 全局打印机状态存储
JsonDocument printerState;

// 看门狗变量
volatile bool watchdogTriggered = false;
unsigned long lastWatchdogFeed = 0;
const long watchdogTimeout = 30000;
unsigned long lastLedProcessTime = 0;
unsigned long lastModeJudgmentTime = 0;

// MQTT 缓冲区文件
const char* MQTT_RX_BUFFER = "/mqtt_rx_buffer.json";
const char* MQTT_TX_BUFFER = "/mqtt_tx_buffer.json";
const size_t MQTT_BUFFER_BLOCK_SIZE = 512; // 每次读写 512 字节

// HTML 静态内容（存储在 PROGMEM）
const char HTML_HEAD[] PROGMEM = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1.0'>
<title>打印机状态显示</title>
<style>
body { font-family: Arial, sans-serif; margin: 0; padding: 16px; background-color: #f4f4f4; }
.container { max-width: 600px; margin: 0 auto; }
.card { background: white; border-radius: 12px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); padding: 16px; margin-bottom: 16px; }
h1 { font-size: 24px; text-align: center; color: #333; }
label { display: block; margin: 8px 0 4px; color: #555; }
input, select { width: 100%; padding: 8px; margin-bottom: 12px; border: 1px solid #ddd; border-radius: 8px; box-sizing: border-box; }
input[type='color'] { width: 40px; height: 40px; border-radius: 50%; border: none; padding: 0; margin-right: 8px; vertical-align: middle; cursor: pointer; }
input[type='checkbox'] { width: auto; margin-right: 8px; }
button { background-color: #2196F3; color: white; padding: 12px; border: none; border-radius: 8px; cursor: pointer; width: 100%; margin-bottom: 8px; font-size: 16px; }
button:hover { background-color: #1976D2; }
.button-group { display: flex; flex-wrap: wrap; gap: 8px; }
.log-container { border: 1px solid #ddd; border-radius: 8px; padding: 8px; height: 200px; overflow-y: auto; background: #fafafa; }
@media (max-width: 600px) { .container { padding: 8px; } .button-group { flex-direction: column; } button { width: 100%; } }
</style>
</head>
<body>
<div class='container'>
<div class='card'>
<h1>打印机状态显示</h1>
<div id='status'>加载中...</div>
<form id='configForm' onsubmit='submitForm(event)'>
)";

const char HTML_FORM[] PROGMEM = R"(
<label for='uid'>用户ID</label>
<input type='text' id='uid' name='uid' value='%s' required>
<label for='accessToken'>访问令牌</label>
<input type='text' id='accessToken' name='accessToken' maxlength='256' value='%s' required>
<label for='deviceID'>设备序列号</label>
<input type='text' id='deviceID' name='deviceID' value='%s' required>
<label for='brightness'>全局亮度（0-255）</label>
<input type='number' id='brightness' name='brightness' min='0' max='255' value='%d' required>
<label for='standbyMode'>待机模式</label>
<select id='standbyMode' name='standbyMode'>
<option value='marquee'%s>彩虹跑马灯</option>
<option value='breathing'%s>呼吸灯</option>
</select>
<label for='progressBarColor'>进度条颜色</label>
<input type='color' id='progressBarColorPicker' name='progressBarColorPicker' value='%s'>
<input type='text' id='progressBarColor' name='progressBarColor' value='%s' pattern='#[0-9A-Fa-f]{6}' required>
<label for='standbyBreathingColor'>待机呼吸灯颜色</label>
<input type='color' id='standbyBreathingColorPicker' name='standbyBreathingColorPicker' value='%s'>
<input type='text' id='standbyBreathingColor' name='standbyBreathingColor' value='%s' pattern='#[0-9A-Fa-f]{6}' required>
<label for='progressBarBrightnessRatio'>进度条亮度比例（0.0-1.0）</label>
<input type='number' id='progressBarBrightnessRatio' name='progressBarBrightnessRatio' min='0' max='1' step='0.1' value='%f' required>
<label for='standbyBrightnessRatio'>待机亮度比例（0.0-1.0）</label>
<input type='number' id='standbyBrightnessRatio' name='standbyBrightnessRatio' min='0' max='1' step='0.1' value='%f' required>
<label for='customPushallInterval'>全量包请求间隔（10-600秒）</label>
<input type='number' id='customPushallInterval' name='customPushallInterval' min='10' max='600' value='%lu' required>
<label><input type='checkbox' id='overlayMarquee' name='overlayMarquee'%s> 在进度条上叠加跑马灯</label>
<button type='submit'>保存配置</button>
</form>
</div>
<div class='card'>
<div class='button-group'>
<button onclick='testLed()'>测试 LED 灯带</button>
<button onclick='clearCache()'>清除缓存</button>
<button onclick='resetConfig()'>重置配置</button>
<button onclick='restart()'>一键重启</button>
</div>
<div style='margin-top: 16px; display: flex; gap: 8px;'>
<select id='switchMode' name='mode'>
<option value='progress'>进度条</option>
<option value='standby'>待机</option>
</select>
<button onclick='switchMode()'>切换模式</button>
</div>
</div>
<div class='card'>
<h2>日志</h2>
<div class='log-container' id='log'>正在加载日志...</div>
</div>
</div>
)";

const char HTML_SCRIPT[] PROGMEM = R"(
<script>
function submitForm(event) {
  event.preventDefault();
  const form = document.getElementById('configForm');
  const formData = new FormData(form);
  fetch('/config', { method: 'POST', body: formData })
    .then(response => {
      if (response.ok) {
        alert('配置保存成功！');
        window.location.reload();
      } else {
        return response.text().then(text => { throw new Error('保存失败：' + text); });
      }
    })
    .catch(error => alert(error.message));
}
function testLed() {
  fetch('/testLed', { method: 'POST' })
    .then(response => {
      if (response.ok) alert('开始测试 LED 灯带！');
      else alert('测试失败');
    })
    .catch(error => alert('测试失败：' + error));
}
function clearCache() {
  fetch('/clearCache', { method: 'POST' })
    .then(response => {
      if (response.ok) alert('缓存已清除！');
      else alert('清除缓存失败');
    })
    .catch(error => alert('清除缓存失败：' + error));
}
function resetConfig() {
  fetch('/resetConfig', { method: 'POST' })
    .then(response => {
      if (response.ok) alert('配置已重置，设备将重启！');
      else alert('重置失败');
    })
    .catch(error => alert('重置失败：' + error));
}
function restart() {
  fetch('/restart', { method: 'POST' })
    .then(response => {
      if (response.ok) alert('设备正在重启！');
      else alert('重启失败');
    })
    .catch(error => alert('重启失败：' + error));
}
function switchMode() {
  const mode = document.getElementById('switchMode').value;
  fetch('/switchMode', { method: 'POST', body: new URLSearchParams({ mode }) })
    .then(response => {
      if (response.ok) alert('模式已切换到 ' + (mode === 'progress' ? '进度条' : '待机') + '！');
      else alert('切换模式失败');
    })
    .catch(error => alert('切换模式失败：' + error));
}
function fetchLog() {
  fetch('/log')
    .then(response => response.text())
    .then(data => {
      document.getElementById('log').innerHTML = data.replace(/<br>/g, '\n');
    })
    .catch(error => {
      document.getElementById('log').innerHTML = '日志加载失败：' + error;
    });
}
setInterval(fetchLog, 5000);
window.onload = function() {
  fetchLog();
  document.getElementById('progressBarColorPicker').addEventListener('input', function() {
    document.getElementById('progressBarColor').value = this.value;
  });
  document.getElementById('progressBarColor').addEventListener('input', function() {
    if (/^#[0-9A-Fa-f]{6}$/.test(this.value)) {
      document.getElementById('progressBarColorPicker').value = this.value;
    }
  });
  document.getElementById('standbyBreathingColorPicker').addEventListener('input', function() {
    document.getElementById('standbyBreathingColor').value = this.value;
  });
  document.getElementById('standbyBreathingColor').addEventListener('input', function() {
    if (/^#[0-9A-Fa-f]{6}$/.test(this.value)) {
      document.getElementById('standbyBreathingColorPicker').value = this.value;
    }
  });
};
</script>
</body>
</html>
)";

// 函数声明
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void loadConfig();
void saveConfig();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void sendPushall();
void processMqttRxBuffer();
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
void handleRestart();
bool isConfigValid();
bool isPrinting();
void watchdogCallback();
void sendFileInChunks(const char* filepath);
void writeMqttRxBuffer(const byte* data, unsigned int length);
void writeMqttTxBuffer(const char* data, size_t length);
bool readMqttBufferBlock(const char* filepath, char* buffer, size_t bufferSize, size_t& offset, size_t& bytesRead);
void initStaticHtml();

// 初始化静态 HTML 文件
void initStaticHtml() {
  if (!LittleFS.exists("/")) {
    if (!LittleFS.mkdir("/")) {
      Serial.println(F("无法创建根目录"));
      return;
    }
    Serial.println(F("根目录已创建"));
  }

  File file = LittleFS.open("/index.html", "w");
  if (!file) {
    Serial.println(F("无法创建 /index.html"));
    return;
  }

  char progressBarColorHex[8];
  char standbyBreathingColorHex[8];
  snprintf(progressBarColorHex, sizeof(progressBarColorHex), "#%06X", progressBarColor);
  snprintf(standbyBreathingColorHex, sizeof(standbyBreathingColorHex), "#%06X", standbyBreathingColor);

  // 写入 HTML_HEAD
  size_t bytesWritten = 0;
  for (size_t i = 0; i < strlen_P(HTML_HEAD); i++) {
    bytesWritten += file.write(pgm_read_byte(HTML_HEAD + i));
  }

  // 写入 HTML_FORM（动态填充）
  char formBuffer[2048];
  snprintf(formBuffer, sizeof(formBuffer),
           PSTR(R"(
<label for='uid'>用户ID</label>
<input type='text' id='uid' name='uid' value='%s' required>
<label for='accessToken'>访问令牌</label>
<input type='text' id='accessToken' name='accessToken' maxlength='256' value='%s' required>
<label for='deviceID'>设备序列号</label>
<input type='text' id='deviceID' name='deviceID' value='%s' required>
<label for='brightness'>全局亮度（0-255）</label>
<input type='number' id='brightness' name='brightness' min='0' max='255' value='%d' required>
<label for='standbyMode'>待机模式</label>
<select id='standbyMode' name='standbyMode'>
<option value='marquee'%s>彩虹跑马灯</option>
<option value='breathing'%s>呼吸灯</option>
</select>
<label for='progressBarColor'>进度条颜色</label>
<input type='color' id='progressBarColorPicker' name='progressBarColorPicker' value='%s'>
<input type='text' id='progressBarColor' name='progressBarColor' value='%s' pattern='#[0-9A-Fa-f]{6}' required>
<label for='standbyBreathingColor'>待机呼吸灯颜色</label>
<input type='color' id='standbyBreathingColorPicker' name='standbyBreathingColorPicker' value='%s'>
<input type='text' id='standbyBreathingColor' name='standbyBreathingColor' value='%s' pattern='#[0-9A-Fa-f]{6}' required>
<label for='progressBarBrightnessRatio'>进度条亮度比例（0.0-1.0）</label>
<input type='number' id='progressBarBrightnessRatio' name='progressBarBrightnessRatio' min='0' max='1' step='0.1' value='%f' required>
<label for='standbyBrightnessRatio'>待机亮度比例（0.0-1.0）</label>
<input type='number' id='standbyBrightnessRatio' name='standbyBrightnessRatio' min='0' max='1' step='0.1' value='%f' required>
<label for='customPushallInterval'>全量包请求间隔（10-600秒）</label>
<input type='number' id='customPushallInterval' name='customPushallInterval' min='10' max='600' value='%lu' required>
<label><input type='checkbox' id='overlayMarquee' name='overlayMarquee'%s> 在进度条上叠加跑马灯</label>
<button type='submit'>保存配置</button>
</form>
</div>
<div class='card'>
<div class='button-group'>
<button onclick='testLed()'>测试 LED 灯带</button>
<button onclick='clearCache()'>清除缓存</button>
<button onclick='resetConfig()'>重置配置</button>
<button onclick='restart()'>一键重启</button>
</div>
<div style='margin-top: 16px; display: flex; gap: 8px;'>
<select id='switchMode' name='mode'>
<option value='progress'>进度条</option>
<option value='standby'>待机</option>
</select>
<button onclick='switchMode()'>切换模式</button>
</div>
</div>
<div class='card'>
<h2>日志</h2>
<div class='log-container' id='log'>正在加载日志...</div>
</div>
</div>
)"),
           uid, accessToken, deviceID, globalBrightness,
           strcmp(standbyMode, "marquee") == 0 ? " selected" : "",
           strcmp(standbyMode, "breathing") == 0 ? " selected" : "",
           progressBarColorHex, progressBarColorHex,
           standbyBreathingColorHex, standbyBreathingColorHex,
           progressBarBrightnessRatio, standbyBrightnessRatio,
           customPushallInterval, overlayMarquee ? " checked" : "");
  bytesWritten += file.print(formBuffer);

  // 写入 HTML_SCRIPT
  for (size_t i = 0; i < strlen_P(HTML_SCRIPT); i++) {
    bytesWritten += file.write(pgm_read_byte(HTML_SCRIPT + i));
  }

  file.close();
  Serial.println(F("index.html 已写入，长度："));
  Serial.println(bytesWritten);
}

// 分块发送文件
void sendFileInChunks(const char* filepath) {
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true; // 暂停 MQTT 处理
  webResponseStartTime = millis();

  if (!LittleFS.exists(filepath)) {
    Serial.println(F("文件不存在："));
    Serial.println(filepath);
    server.send(500, "text/plain", "文件不存在");
    isWebServing = false;
    pauseLedUpdate = false;
    pauseMqttUpdate = false;
    return;
  }

  size_t chunkSize = 256; // 减小分块大小以降低内存占用
  if (ESP.getFreeHeap() < 8000) {
    Serial.println(F("堆内存低（"));
    Serial.print(ESP.getFreeHeap());
    Serial.println(F("字节），使用 PROGMEM 缓冲区"));
    char progressBarColorHex[8];
    char standbyBreathingColorHex[8];
    snprintf(progressBarColorHex, sizeof(progressBarColorHex), "#%06X", progressBarColor);
    snprintf(standbyBreathingColorHex, sizeof(standbyBreathingColorHex), "#%06X", standbyBreathingColor);

    // 直接从 PROGMEM 发送 HTML
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");

    unsigned long startTime = millis();
    // 发送 HTML_HEAD
    char buffer[256];
    size_t headLen = strlen_P(HTML_HEAD);
    for (size_t i = 0; i < headLen; i += chunkSize) {
      size_t len = min(chunkSize, headLen - i);
      for (size_t j = 0; j < len; j++) {
        buffer[j] = pgm_read_byte(HTML_HEAD + i + j);
      }
      server.client().write(buffer, len);
      yield();
      if (millis() - startTime > 2000) {
        Serial.println(F("Web 响应超时（低内存模式）"));
        server.client().stop();
        isWebServing = false;
        pauseLedUpdate = false;
        pauseMqttUpdate = false;
        return;
      }
    }

    // 发送 HTML_FORM
    char formBuffer[2048];
    snprintf(formBuffer, sizeof(formBuffer),
             PSTR(R"(
<label for='uid'>用户ID</label>
<input type='text' id='uid' name='uid' value='%s' required>
<label for='accessToken'>访问令牌</label>
<input type='text' id='accessToken' name='accessToken' maxlength='256' value='%s' required>
<label for='deviceID'>设备序列号</label>
<input type='text' id='deviceID' name='deviceID' value='%s' required>
<label for='brightness'>全局亮度（0-255）</label>
<input type='number' id='brightness' name='brightness' min='0' max='255' value='%d' required>
<label for='standbyMode'>待机模式</label>
<select id='standbyMode' name='standbyMode'>
<option value='marquee'%s>彩虹跑马灯</option>
<option value='breathing'%s>呼吸灯</option>
</select>
<label for='progressBarColor'>进度条颜色</label>
<input type='color' id='progressBarColorPicker' name='progressBarColorPicker' value='%s'>
<input type='text' id='progressBarColor' name='progressBarColor' value='%s' pattern='#[0-9A-Fa-f]{6}' required>
<label for='standbyBreathingColor'>待机呼吸灯颜色</label>
<input type='color' id='standbyBreathingColorPicker' name='standbyBreathingColorPicker' value='%s'>
<input type='text' id='standbyBreathingColor' name='standbyBreathingColor' value='%s' pattern='#[0-9A-Fa-f]{6}' required>
<label for='progressBarBrightnessRatio'>进度条亮度比例（0.0-1.0）</label>
<input type='number' id='progressBarBrightnessRatio' name='progressBarBrightnessRatio' min='0' max='1' step='0.1' value='%f' required>
<label for='standbyBrightnessRatio'>待机亮度比例（0.0-1.0）</label>
<input type='number' id='standbyBrightnessRatio' name='standbyBrightnessRatio' min='0' max='1' step='0.1' value='%f' required>
<label for='customPushallInterval'>全量包请求间隔（10-600秒）</label>
<input type='number' id='customPushallInterval' name='customPushallInterval' min='10' max='600' value='%lu' required>
<label><input type='checkbox' id='overlayMarquee' name='overlayMarquee'%s> 在进度条上叠加跑马灯</label>
<button type='submit'>保存配置</button>
</form>
</div>
<div class='card'>
<div class='button-group'>
<button onclick='testLed()'>测试 LED 灯带</button>
<button onclick='clearCache()'>清除缓存</button>
<button onclick='resetConfig()'>重置配置</button>
<button onclick='restart()'>一键重启</button>
</div>
<div style='margin-top: 16px; display: flex; gap: 8px;'>
<select id='switchMode' name='mode'>
<option value='progress'>进度条</option>
<option value='standby'>待机</option>
</select>
<button onclick='switchMode()'>切换模式</button>
</div>
</div>
<div class='card'>
<h2>日志</h2>
<div class='log-container' id='log'>正在加载日志...</div>
</div>
</div>
)"),
             uid, accessToken, deviceID, globalBrightness,
             strcmp(standbyMode, "marquee") == 0 ? " selected" : "",
             strcmp(standbyMode, "breathing") == 0 ? " selected" : "",
             progressBarColorHex, progressBarColorHex,
             standbyBreathingColorHex, standbyBreathingColorHex,
             progressBarBrightnessRatio, standbyBrightnessRatio,
             customPushallInterval, overlayMarquee ? " checked" : "");
    size_t formLen = strlen(formBuffer);
    for (size_t i = 0; i < formLen; i += chunkSize) {
      size_t len = min(chunkSize, formLen - i);
      server.client().write(formBuffer + i, len);
      yield();
      if (millis() - startTime > 2000) {
        Serial.println(F("Web 响应超时（低内存模式）"));
        server.client().stop();
        isWebServing = false;
        pauseLedUpdate = false;
        pauseMqttUpdate = false;
        return;
      }
    }

    // 发送 HTML_SCRIPT
    size_t scriptLen = strlen_P(HTML_SCRIPT);
    for (size_t i = 0; i < scriptLen; i += chunkSize) {
      size_t len = min(chunkSize, scriptLen - i);
      for (size_t j = 0; j < len; j++) {
        buffer[j] = pgm_read_byte(HTML_SCRIPT + i + j);
      }
      server.client().write(buffer, len);
      yield();
      if (millis() - startTime > 2000) {
        Serial.println(F("Web 响应超时（低内存模式）"));
        server.client().stop();
        isWebServing = false;
        pauseLedUpdate = false;
        pauseMqttUpdate = false;
        return;
      }
    }

    Serial.println(F("文件发送完成（低内存模式）："));
    Serial.println(filepath);
  } else {
    File file = LittleFS.open(filepath, "r");
    if (!file) {
      Serial.println(F("无法打开文件："));
      Serial.println(filepath);
      server.send(500, "text/plain", "无法打开文件");
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }

    size_t fileSize = file.size();
    server.setContentLength(fileSize);
    server.send(200, "text/html; charset=utf-8", "");

    char buffer[256];
    unsigned long startTime = millis();
    while (file.available()) {
      size_t bytesRead = file.readBytes(buffer, min(chunkSize, static_cast<size_t>(file.available())));
      server.client().write(buffer, bytesRead);
      yield();
      if (millis() - startTime > 2000) {
        Serial.println(F("Web 响应超时（文件模式）"));
        file.close();
        server.client().stop();
        isWebServing = false;
        pauseLedUpdate = false;
        pauseMqttUpdate = false;
        return;
      }
    }
    file.close();
    Serial.println(F("文件发送完成（文件模式）："));
    Serial.println(filepath);
  }

  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
  if (pendingPushall) {
    sendPushall();
    pendingPushall = false;
  }
}

// 看门狗中断处理
void watchdogCallback() {
  if (isWebServing) return;

  unsigned long currentMillis = millis();
  
  if (lastLedProcessTime > 0 && currentMillis - lastLedProcessTime > watchdogTimeout) {
    Serial.println(F("看门狗：LED 渲染进程卡死！"));
    watchdogTriggered = true;
  }
  
  if (!printerOffline && lastModeJudgmentTime > 0 && currentMillis - lastModeJudgmentTime > watchdogTimeout) {
    Serial.println(F("看门狗：模式判断进程卡死！当前状态："));
    Serial.println(currentState);
    watchdogTriggered = true;
  }
  
  if (ESP.getFreeHeap() < 300) {
    Serial.println(F("看门狗：堆内存低于 300 字节（剩余 "));
    Serial.print(ESP.getFreeHeap());
    Serial.println(F("）！"));
    watchdogTriggered = true;
  }
  
  if (watchdogTriggered) {
    Serial.println(F("看门狗触发！系统将重启。"));
    delay(1000);
    ESP.restart();
  }
}

// 写入 MQTT 接收缓冲区
void writeMqttRxBuffer(const byte* data, unsigned int length) {
  File file = LittleFS.open(MQTT_RX_BUFFER, "a");
  if (!file) {
    Serial.println(F("无法打开 MQTT 接收缓冲区文件"));
    return;
  }
  // 写入长度前缀（4 字节）
  file.write(reinterpret_cast<const char*>(&length), sizeof(length));
  // 写入数据
  size_t bytesWritten = file.write(reinterpret_cast<const char*>(data), length);
  file.close();
  if (bytesWritten != length) {
    Serial.println(F("写入 MQTT 接收缓冲区失败，写入字节："));
    Serial.println(bytesWritten);
  } else {
    Serial.println(F("写入 MQTT 接收缓冲区，长度："));
    Serial.println(length);
  }
}

// 写入 MQTT 发送缓冲区
void writeMqttTxBuffer(const char* data, size_t length) {
  File file = LittleFS.open(MQTT_TX_BUFFER, "a");
  if (!file) {
    Serial.println(F("无法打开 MQTT 发送缓冲区文件"));
    return;
  }
  // 写入长度前缀（4 字节）
  file.write(reinterpret_cast<const char*>(&length), sizeof(length));
  // 写入数据
  size_t bytesWritten = file.write(data, length);
  file.close();
  if (bytesWritten != length) {
    Serial.println(F("写入 MQTT 发送缓冲区失败，写入字节："));
    Serial.println(bytesWritten);
  } else {
    Serial.println(F("写入 MQTT 发送缓冲区，长度："));
    Serial.println(length);
  }
}

// 分块读取缓冲区
bool readMqttBufferBlock(const char* filepath, char* buffer, size_t bufferSize, size_t& offset, size_t& bytesRead) {
  File file = LittleFS.open(filepath, "r");
  if (!file) {
    Serial.println(F("无法打开缓冲区文件："));
    Serial.println(filepath);
    return false;
  }
  if (!file.seek(offset)) {
    Serial.println(F("无法定位文件偏移："));
    Serial.println(offset);
    file.close();
    return false;
  }
  // 读取长度前缀
  size_t length;
  if (file.readBytes(reinterpret_cast<char*>(&length), sizeof(length)) != sizeof(length)) {
    file.close();
    return false;
  }
  if (length > bufferSize) {
    Serial.println(F("消息块过大："));
    Serial.println(length);
    file.close();
    return false;
  }
  // 读取数据
  bytesRead = file.readBytes(buffer, length);
  offset += sizeof(length) + bytesRead;
  file.close();
  if (bytesRead != length) {
    Serial.println(F("读取缓冲区失败，预期："));
    Serial.print(length);
    Serial.print(F("，实际："));
    Serial.println(bytesRead);
    return false;
  }
  return true;
}

// 处理 MQTT 接收缓冲区
void processMqttRxBuffer() {
  if (pauseMqttUpdate || !LittleFS.exists(MQTT_RX_BUFFER)) {
    return;
  }

  size_t offset = 0;
  char buffer[MQTT_BUFFER_BLOCK_SIZE];
  size_t bytesRead;

  while (readMqttBufferBlock(MQTT_RX_BUFFER, buffer, MQTT_BUFFER_BLOCK_SIZE, offset, bytesRead)) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, buffer, bytesRead);
    if (error) {
      Serial.println(F("JSON 解析失败："));
      Serial.println(error.c_str());
      continue;
    }

    if (doc["print"].isNull()) {
      Serial.print(F("收到非 print 消息：command="));
      Serial.println(doc["command"] | "unknown");
      continue;
    }

    lastMqttResponseTime = millis();
    JsonObject printData = doc["print"];
    if (doc["command"] && doc["command"] == "pushall") {
      printerState.clear();
      for (JsonPair kv : printData) {
        printerState[kv.key()] = kv.value();
      }
      Serial.println(F("全量更新完成"));
    } else {
      for (JsonPair kv : printData) {
        printerState[kv.key()] = kv.value();
      }
      Serial.println(F("增量更新完成"));
    }

    gcodeState = printerState["gcode_state"] | "UNKNOWN";
    printPercent = printerState["mc_percent"] | 0;
    remainingTime = printerState["mc_remaining_time"] | 0;
    layerNum = printerState["layer_num"] | 0;

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
    Serial.print(F("打印状态："));
    Serial.print(gcodeState);
    Serial.print(F("，进度："));
    Serial.print(printPercent);
    Serial.print(F("%，剩余时间："));
    Serial.print(remainingTime);
    Serial.println(F(" 分钟"));
    yield();
  }

  // 清空已处理的文件
  File file = LittleFS.open(MQTT_RX_BUFFER, "r");
  if (file && offset >= file.size()) {
    file.close();
    LittleFS.remove(MQTT_RX_BUFFER);
    Serial.println(F("MQTT 接收缓冲区已清空"));
  } else if (file) {
    file.close();
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
  delay(100);

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
      Serial.print(F("LED 初始化失败，索引："));
      Serial.println(i);
      ledOk = false;
    }
  }
  if (!ledOk) {
    Serial.println(F("LED 灯带初始化失败，继续运行"));
  } else {
    Serial.println(F("LED 灯带初始化成功"));
  }

  if (!LittleFS.begin()) {
    Serial.println(F("无法挂载 LittleFS"));
    return;
  }
  FSInfo fs_info;
  LittleFS.info(fs_info);
  Serial.print(F("LittleFS 挂载成功，剩余空间："));
  Serial.print(fs_info.totalBytes - fs_info.usedBytes);
  Serial.println(F(" 字节"));

  // 初始化 MQTT 缓冲区文件
  if (LittleFS.exists(MQTT_RX_BUFFER)) {
    LittleFS.remove(MQTT_RX_BUFFER);
  }
  if (LittleFS.exists(MQTT_TX_BUFFER)) {
    LittleFS.remove(MQTT_TX_BUFFER);
  }

  loadConfig();
  initStaticHtml();

  WiFiManagerParameter custom_uid("uid", "用户ID", uid, 32);
  WiFiManagerParameter custom_access_token("accessToken", "访问令牌", accessToken, 256);
  WiFiManagerParameter custom_device_id("deviceID", "设备序列号", deviceID, 32);
  wm.addParameter(&custom_uid);
  wm.addParameter(&custom_access_token);
  wm.addParameter(&custom_device_id);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);

  currentState = CONNECTING_WIFI;
  lastModeJudgmentTime = millis();
  if (!wm.autoConnect("BambuAP")) {
    Serial.println(F("WiFi 连接失败，进入 AP 模式"));
    currentState = AP_MODE;
    lastModeJudgmentTime = millis();
  } else {
    Serial.println(F("WiFi 连接成功"));
    currentState = CONNECTED_WIFI;
    lastModeJudgmentTime = millis();
    strncpy(uid, custom_uid.getValue(), sizeof(uid) - 1);
    uid[sizeof(uid) - 1] = '\0';
    strncpy(accessToken, custom_access_token.getValue(), sizeof(accessToken) - 1);
    accessToken[sizeof(accessToken) - 1] = '\0';
    strncpy(deviceID, custom_device_id.getValue(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    saveConfig();

    configTime(0, 0, "pool.ntp.org");
    time_t now = time(nullptr);
    unsigned long start = millis();
    while (now < 1000000000 && millis() - start < 5000) {
      delay(500);
      now = time(nullptr);
    }
    if (now >= 1000000000) {
      Serial.println(F("时间同步成功"));
    } else {
      Serial.println(F("NTP 同步失败，继续使用本地时间"));
    }
  }

  if (isConfigValid()) {
    espClient.setInsecure();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
  } else {
    Serial.println(F("配置为空，跳过 MQTT 初始化"));
  }

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/testLed", handleTestLed);
  server.on("/log", handleLog);
  server.on("/clearCache", handleClearCache);
  server.on("/resetConfig", handleResetConfig);
  server.on("/switchMode", handleSwitchMode);
  server.on("/restart", handleRestart);
  server.onNotFound([]() {
    Serial.print(F("未找到页面："));
    Serial.println(server.uri());
    server.send(404, "text/plain", "页面不存在");
  });
  server.begin();
  Serial.println(F("HTTP 服务器启动"));

  watchdogTicker.attach_ms(1000, watchdogCallback);
  lastWatchdogFeed = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastOperationCheck > operationCheckInterval) {
    lastOperationCheck = currentMillis;
    if (WiFi.status() == WL_CONNECTED || currentState == AP_MODE) {
      lastWatchdogFeed = currentMillis;
    }
    lastModeJudgmentTime = currentMillis;
    Serial.print(F("状态："));
    Serial.print(currentState);
    Serial.print(F("，堆内存："));
    Serial.println(ESP.getFreeHeap());
  }

  server.handleClient();

  if (!pauseMqttUpdate && currentState != AP_MODE && WiFi.status() == WL_CONNECTED && isConfigValid()) {
    if (!mqttClient.connected()) {
      currentState = CONNECTING_PRINTER;
      lastModeJudgmentTime = millis();
      reconnectMQTT();
    } else {
      mqttClient.loop();
      if (currentState == CONNECTED_PRINTER && currentMillis - lastMqttResponseTime > mqttResponseTimeout) {
        Serial.println(F("MQTT 响应超时，标记打印机离线"));
        printerOffline = true;
      }
    }
  }

  // 处理接收缓冲区
  if (!pauseMqttUpdate) {
    processMqttRxBuffer();
  }

  // 发送定时 pushall
  if (!isWebServing && !pauseMqttUpdate && currentState == CONNECTED_PRINTER && currentMillis - lastPushallTime > customPushallInterval * 1000) {
    sendPushall();
    lastPushallTime = currentMillis;
  } else if (isWebServing && currentState == CONNECTED_PRINTER && currentMillis - lastPushallTime > customPushallInterval * 1000) {
    pendingPushall = true;
  }

  // 处理待发送的 pushall
  if (!isWebServing && !pauseMqttUpdate && pendingPushall && mqttClient.connected()) {
    sendPushall();
    pendingPushall = false;
  }

  if (activeClientIP != IPAddress(0, 0, 0, 0) && currentMillis - activeClientTimeout > clientTimeoutInterval) {
    activeClientIP = IPAddress(0, 0, 0, 0);
    Serial.println(F("Web 客户端超时，释放锁定"));
  }

  if (isWebServing && currentMillis - webResponseStartTime > webResponseTimeout) {
    Serial.println(F("Web 响应超时，系统将重启"));
    delay(1000);
    ESP.restart();
  }

  if (!pauseLedUpdate) {
    updateLED();
    updateTestLed();
  }

  if (ESP.getFreeHeap() < 8000 && currentMillis - lastHeapWarningTime > 2000) {
    Serial.print(F("警告：堆内存低，仅剩 "));
    Serial.print(ESP.getFreeHeap());
    Serial.println(F(" 字节"));
    lastHeapWarningTime = currentMillis;
  }

  if (currentState != lastState) {
    lastModeJudgmentTime = millis();
    lastState = currentState;
    Serial.print(F("模式变更为："));
    Serial.println(currentState);
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  currentState = AP_MODE;
  lastModeJudgmentTime = millis();
  apClientConnected = false;
  Serial.println(F("进入 AP 模式"));
}

void saveConfigCallback() {
  saveConfig();
}

void loadConfig() {
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("未找到配置文件，使用默认值"));
    return;
  }
  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println(F("配置文件过大"));
    configFile.close();
    return;
  }
  char buf[1024];
  configFile.readBytes(buf, size);
  configFile.close();
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buf);
  if (error) {
    Serial.print(F("解析配置文件失败："));
    Serial.println(error.c_str());
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
  customPushallInterval = doc["customPushallInterval"] | 30;
  strip.setBrightness(constrain(globalBrightness, 0, 255));
}

void saveConfig() {
  JsonDocument doc;
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
  doc["customPushallInterval"] = customPushallInterval;
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println(F("无法写入配置文件"));
    return;
  }
  serializeJson(doc, configFile);
  configFile.close();
  sendPushall();
}

void reconnectMQTT() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastReconnectAttempt < reconnectDelay) return;
  lastReconnectAttempt = currentMillis;

  String clientID = "ESP8266Client-" + String(random(0xffff), HEX);
  Serial.print(F("连接到 MQTT 服务器："));
  Serial.print(MQTT_SERVER);
  Serial.print(F("，客户端 ID："));
  Serial.println(clientID);
  if (mqttClient.connect(clientID.c_str(), uid, accessToken)) {
    char topicSub[64];
    strcpy_P(topicSub, MQTT_TOPIC_SUB);
    String topic = String(topicSub).c_str();
    topic.replace("{DEVICE_ID}", deviceID);
    mqttClient.subscribe(topic.c_str());
    currentState = CONNECTING_PRINTER;
    lastModeJudgmentTime = millis();
    printerOffline = false;
    Serial.print(F("MQTT 连接成功，订阅主题："));
    Serial.println(topic);
    sendPushall();
    reconnectDelay = 1000;
  } else {
    Serial.print(F("MQTT 连接失败，错误码="));
    Serial.println(mqttClient.state());
    reconnectDelay = min(reconnectDelay * 2, maxReconnectDelay);
    Serial.print(F("将在 "));
    Serial.print(reconnectDelay / 1000);
    Serial.println(F(" 秒后重试"));
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!pauseMqttUpdate) {
    writeMqttRxBuffer(payload, length);
    lastMqttResponseTime = millis();
  }
}

void sendPushall() {
  JsonDocument pushall_request;
  pushall_request["pushing"]["sequence_id"] = String(millis());
  pushall_request["pushing"]["command"] = "pushall";
  pushall_request["pushing"]["version"] = 1;
  pushall_request["pushing"]["push_target"] = 1;
  char payload[256];
  size_t length = serializeJson(pushall_request, payload, sizeof(payload));
  writeMqttTxBuffer(payload, length);

  size_t offset = 0;
  char buffer[MQTT_BUFFER_BLOCK_SIZE];
  size_t bytesRead;
  char topicPub[64];
  strcpy_P(topicPub, MQTT_TOPIC_PUB);
  String topic = String(topicPub).c_str();
  topic.replace("{DEVICE_ID}", deviceID);

  while (readMqttBufferBlock(MQTT_TX_BUFFER, buffer, MQTT_BUFFER_BLOCK_SIZE, offset, bytesRead)) {
    if (mqttClient.publish(topic.c_str(), (const uint8_t*)buffer, bytesRead, true)) {
      Serial.print(F("发送 pushall 请求到 "));
      Serial.print(topic);
      Serial.print(F("，长度："));
      Serial.println(bytesRead);
    } else {
      Serial.println(F("发送 pushall 请求失败"));
      pendingPushall = true;
      break;
    }
    yield();
  }

  // 清空发送缓冲区
  File file = LittleFS.open(MQTT_TX_BUFFER, "r");
  if (file && offset >= file.size()) {
    file.close();
    LittleFS.remove(MQTT_TX_BUFFER);
    Serial.println(F("MQTT 发送缓冲区已清空"));
  } else if (file) {
    file.close();
  }
}

uint32_t getRainbowColor(float position) {
  position = fmod(position, 1.0);
  if (position < 0.166) return strip.Color(255, 0, 255 * (position / 0.166));
  else if (position < 0.333) return strip.Color(255 * (1 - (position - 0.166) / 0.166), 255 * ((position - 0.166) / 0.166), 0);
  else if (position < 0.5) return strip.Color(0, 255, 255 * (1 - (position - 0.333) / 0.166));
  else if (position < 0.666) return strip.Color(255 * ((position - 0.5) / 0.166), 255 * (1 - (position - 0.5) / 0.166), 0);
  else if (position < 0.833) return strip.Color(255, 0, 255 * ((position - 0.666) / 0.166));
  else return strip.Color(255 * (1 - (position - 0.833) / 0.166), 0, 255);
}

void updateLED() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < ledUpdateInterval || pauseLedUpdate) return;
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
  if (!testingLed || pauseLedUpdate) return;
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
    Serial.print(F("新客户端连接："));
    Serial.println(clientIP.toString());
    return true;
  } else if (clientIP == activeClientIP) {
    activeClientTimeout = millis();
    return true;
  } else {
    Serial.print(F("拒绝客户端："));
    Serial.println(clientIP.toString());
    return false;
  }
}

void handleRoot() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }

  sendFileInChunks("/index.html");
}

void handleConfig() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }

  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();

  if (server.hasArg("uid") && server.hasArg("accessToken") && server.hasArg("deviceID")) {
    String newUid = server.arg("uid");
    String newAccessToken = server.arg("accessToken");
    String newDeviceID = server.arg("deviceID");
    int newBrightness = server.arg("brightness").toInt();
    String newStandbyMode = server.arg("standbyMode");
    String newProgressBarColor = server.arg("progressBarColor");
    String newStandbyBreathingColor = server.arg("standbyBreathingColor");
    float newPbr = server.arg("progressBarBrightnessRatio").toFloat();
    float newSbr = server.arg("standbyBrightnessRatio").toFloat();
    int newPushallInterval = server.arg("customPushallInterval").toInt();
    bool newOverlayMarquee = server.hasArg("overlayMarquee") && server.arg("overlayMarquee") == "on";

    if (newUid.length() == 0 || newAccessToken.length() == 0 || newDeviceID.length() == 0) {
      String response = F("<script>alert('用户ID、访问令牌和设备序列号不能为空！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }
    if (newBrightness < 0 || newBrightness > 255) {
      String response = F("<script>alert('全局亮度必须在 0 到 255 之间！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }
    if (newPbr < 0.0 || newPbr > 1.0 || newSbr < 0.0 || newSbr > 1.0) {
      String response = F("<script>alert('亮度比例必须在 0.0 到 1.0 之间！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }
    if (newPushallInterval < 10 || newPushallInterval > 600) {
      String response = F("<script>alert('全量包请求间隔必须在 10 到 600 秒之间！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }
    if (!newProgressBarColor.startsWith("#") || newProgressBarColor.length() != 7) {
      String response = F("<script>alert('进度条颜色格式无效，必须为 #RRGGBB！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }
    if (!newStandbyBreathingColor.startsWith("#") || newStandbyBreathingColor.length() != 7) {
      String response = F("<script>alert('待机呼吸灯颜色格式无效，必须为 #RRGGBB！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
      isWebServing = false;
      pauseLedUpdate = false;
      pauseMqttUpdate = false;
      return;
    }

    strncpy(uid, newUid.c_str(), sizeof(uid) - 1);
    uid[sizeof(uid) - 1] = '\0';
    strncpy(accessToken, newAccessToken.c_str(), sizeof(accessToken) - 1);
    accessToken[sizeof(accessToken) - 1] = '\0';
    strncpy(deviceID, newDeviceID.c_str(), sizeof(deviceID) - 1);
    deviceID[sizeof(deviceID) - 1] = '\0';
    globalBrightness = newBrightness;
    strncpy(standbyMode, newStandbyMode.c_str(), sizeof(standbyMode) - 1);
    standbyMode[sizeof(standbyMode) - 1] = '\0';
    overlayMarquee = newOverlayMarquee;
    progressBarBrightnessRatio = newPbr;
    standbyBrightnessRatio = newSbr;
    customPushallInterval = newPushallInterval;

    // 转换颜色值
    unsigned long progressBarColorValue = strtoul(newProgressBarColor.substring(1).c_str(), NULL, 16);
    progressBarColor = strip.Color((progressBarColorValue >> 16) & 0xFF, (progressBarColorValue >> 8) & 0xFF, progressBarColorValue & 0xFF);
    unsigned long standbyBreathingColorValue = strtoul(newStandbyBreathingColor.substring(1).c_str(), NULL, 16);
    standbyBreathingColor = strip.Color((standbyBreathingColorValue >> 16) & 0xFF, (standbyBreathingColorValue >> 8) & 0xFF, standbyBreathingColorValue & 0xFF);

    strip.setBrightness(globalBrightness);
    saveConfig();
    initStaticHtml();

    // 更新 MQTT 配置
    mqttClient.disconnect();
    reconnectMQTT();

    String response = F("<script>alert('配置保存成功！'); window.location.href='/';</script>");
    server.send(200, "text/html; charset=utf-8", response);
  } else {
    String response = F("<script>alert('缺少必要参数！'); window.location.href='/';</script>");
    server.send(400, "text/html; charset=utf-8", response);
  }
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
}

void handleTestLed() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();
  testingLed = true;
  testLedIndex = 0;
  String response = F("<script>alert('开始测试 LED 灯带！'); window.location.href='/';</script>");
  server.send(200, "text/html; charset=utf-8", response);
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
}

void handleLog() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();
  char logContent[512];
  char progressBarColorHex[8];
  char standbyBreathingColorHex[8];
  snprintf(progressBarColorHex, sizeof(progressBarColorHex), "#%06X", progressBarColor);
  snprintf(standbyBreathingColorHex, sizeof(standbyBreathingColorHex), "#%06X", standbyBreathingColor);
  snprintf(logContent, sizeof(logContent),
           "当前状态: %d<br>"
           "WiFi 状态: %s<br>"
           "MQTT 状态: %s<br>"
           "打印机状态: %s<br>"
           "打印进度: %d%%<br>"
           "剩余时间: %d 分钟<br>"
           "当前层数: %d<br>"
           "堆内存: %u 字节<br>"
           "进度条颜色: %s<br>"
           "待机呼吸灯颜色: %s<br>",
           currentState,
           WiFi.status() == WL_CONNECTED ? "已连接" : "未连接",
           mqttClient.connected() ? "已连接" : "未连接",
           gcodeState.c_str(),
           printPercent,
           remainingTime,
           layerNum,
           ESP.getFreeHeap(),
           progressBarColorHex,
           standbyBreathingColorHex);
  server.send(200, "text/html; charset=utf-8", logContent);
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
}

void handleClearCache() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();
  if (LittleFS.exists(MQTT_RX_BUFFER)) {
    LittleFS.remove(MQTT_RX_BUFFER);
  }
  if (LittleFS.exists(MQTT_TX_BUFFER)) {
    LittleFS.remove(MQTT_TX_BUFFER);
  }
  String response = F("<script>alert('缓存已清除！'); window.location.href='/';</script>");
  server.send(200, "text/html; charset=utf-8", response);
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
}

void handleResetConfig() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();
  if (LittleFS.exists("/config.json")) {
    LittleFS.remove("/config.json");
  }
  memset(uid, 0, sizeof(uid));
  memset(accessToken, 0, sizeof(accessToken));
  memset(deviceID, 0, sizeof(deviceID));
  globalBrightness = 50;
  strcpy(standbyMode, "marquee");
  overlayMarquee = false;
  progressBarColor = strip.Color(0, 255, 0);
  standbyBreathingColor = strip.Color(0, 0, 255);
  progressBarBrightnessRatio = 1.0;
  standbyBrightnessRatio = 1.0;
  customPushallInterval = 30;
  strip.setBrightness(globalBrightness);
  saveConfig();
  initStaticHtml();
  String response = F("<script>alert('配置已重置，设备将重启！'); window.location.href='/';</script>");
  server.send(200, "text/html; charset=utf-8", response);
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
  delay(1000);
  ESP.restart();
}

void handleSwitchMode() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "progress") {
      forcedMode = PROGRESS;
      currentState = PRINTING;
      lastModeJudgmentTime = millis();
      String response = F("<script>alert('已切换到进度条模式！'); window.location.href='/';</script>");
      server.send(200, "text/html; charset=utf-8", response);
    } else if (mode == "standby") {
      forcedMode = STANDBY;
      currentState = CONNECTED_PRINTER;
      lastModeJudgmentTime = millis();
      String response = F("<script>alert('已切换到待机模式！'); window.location.href='/';</script>");
      server.send(200, "text/html; charset=utf-8", response);
    } else {
      String response = F("<script>alert('无效的模式！'); window.location.href='/';</script>");
      server.send(400, "text/html; charset=utf-8", response);
    }
  } else {
    String response = F("<script>alert('缺少模式参数！'); window.location.href='/';</script>");
    server.send(400, "text/html; charset=utf-8", response);
  }
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
}

void handleRestart() {
  if (!checkClientAccess()) {
    server.send(403, "text/html", "<h1>访问被拒绝</h1><p>另一个设备正在配置。</p>");
    return;
  }
  isWebServing = true;
  pauseLedUpdate = true;
  pauseMqttUpdate = true;
  webResponseStartTime = millis();
  String response = F("<script>alert('设备正在重启！'); window.location.href='/';</script>");
  server.send(200, "text/html; charset=utf-8", response);
  isWebServing = false;
  pauseLedUpdate = false;
  pauseMqttUpdate = false;
  delay(1000);
  ESP.restart();
}