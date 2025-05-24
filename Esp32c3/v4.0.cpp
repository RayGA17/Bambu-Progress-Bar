#include <Arduino.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <pgmspace.h>
#include <time.h>

// --- Configuration ---
#define LED_PIN 8         // ESP32-C3 Mini-1-H4 的 GPIO8
#define LED_COUNT 20      // Number of LEDs in the strip
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// MQTT Server Configuration (Stored in PROGMEM)
const char MQTT_SERVER[] PROGMEM = "cn.mqtt.bambulab.com";
const int MQTT_PORT = 8883;

// MQTT Topics (Stored in PROGMEM)
const char MQTT_TOPIC_SUB_TEMPLATE[] PROGMEM = "device/{DEVICE_ID}/report";
const char MQTT_TOPIC_PUB_TEMPLATE[] PROGMEM = "device/{DEVICE_ID}/request";

// Filenames for MQTT Buffering
const char* MQTT_RX_BUFFER_FILE = "/mqtt_rx_buffer.json";
const char* MQTT_TX_BUFFER_FILE = "/mqtt_tx_buffer.json";
const size_t MQTT_BUFFER_BLOCK_SIZE = 512;

// Watchdog Timeout (seconds)
#define WATCHDOG_TIMEOUT 60

// --- Global Objects ---
AsyncWebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
WiFiManager wm;

// --- Configuration Variables ---
char uid[32] = "";
char accessToken[256] = "";
char deviceID[32] = "";
int globalBrightness = 50;
char standbyMode[10] = "marquee";
bool overlayMarquee = false;
uint32_t progressBarColor = strip.Color(0, 255, 0);
uint32_t standbyBreathingColor = strip.Color(0, 0, 255);
float progressBarBrightnessRatio = 1.0;
float standbyBrightnessRatio = 1.0;
unsigned long customPushallInterval = 30;

// --- State Variables ---
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
String gcodeState = "UNKNOWN";
int remainingTime = 0;
int layerNum = 0;
DynamicJsonDocument printerState(4096);

// Forced Mode Control
enum ForcedMode {
  NONE,
  PROGRESS,
  STANDBY
};
ForcedMode forcedMode = NONE;

// --- Timing and Control Flags ---
unsigned long lastReconnectAttempt = 0;
unsigned long reconnectDelay = 1000;
const unsigned long MAX_RECONNECT_DELAY = 120000;
unsigned long lastWiFiCheck = 0;
const long WIFI_CHECK_INTERVAL = 10000;
unsigned long lastPushallTime = 0;
unsigned long lastOperationCheck = 0;
const long OPERATION_CHECK_INTERVAL = 5000;
unsigned long lastLedUpdate = 0;
const long LED_UPDATE_INTERVAL = 16;
int marqueePosition = 0;
bool apClientConnected = false;
bool pendingPushall = false;
unsigned long lastMqttMessageTime = 0;
const unsigned long MQTT_TIMEOUT = 300000;

bool testingLed = false;
int testLedIndex = 0;
unsigned long lastTestLedUpdate = 0;
const long TEST_LED_INTERVAL = 50;

// --- HTML Content (Stored in PROGMEM) ---
const char HTML_HEAD[] PROGMEM = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1.0'>
<title>打印机状态显示</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif; margin: 0; padding: 16px; background-color: #f0f2f5; font-size: 14px; color: #333; }
.container { max-width: 600px; margin: 0 auto; }
.card { background: white; border-radius: 12px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); padding: 20px; margin-bottom: 16px; }
h1 { font-size: 22px; text-align: center; color: #111; margin-bottom: 20px; }
h2 { font-size: 18px; color: #333; margin-top: 20px; margin-bottom: 10px; border-bottom: 1px solid #eee; padding-bottom: 5px;}
label { display: block; margin: 10px 0 5px; color: #555; font-weight: 500; }
input[type='text'], input[type='number'], select { width: 100%; padding: 10px; margin-bottom: 12px; border: 1px solid #ccc; border-radius: 8px; box-sizing: border-box; font-size: 14px; }
input[type='color'] { min-width: 40px; height: 40px; border-radius: 8px; border: 1px solid #ccc; padding: 0; margin-right: 8px; vertical-align: middle; cursor: pointer; }
input[type='checkbox'] { width: auto; margin-right: 8px; vertical-align: middle; }
button { background-color: #007aff; color: white; padding: 12px 18px; border: none; border-radius: 8px; cursor: pointer; width: 100%; margin-bottom: 10px; font-size: 15px; font-weight: 500; transition: background-color 0.2s ease; }
button:hover { background-color: #0056b3; }
.button-group { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
.log-container { border: 1px solid #ddd; border-radius: 8px; padding: 12px; height: 200px; overflow-y: auto; background: #f8f9fa; font-family: monospace; font-size: 12px; white-space: pre-wrap; word-wrap: break-word; }
#status { background-color: #e9ecef; padding: 10px; border-radius: 8px; margin-bottom: 15px; font-weight: 500; }
.color-picker-group { display: flex; align-items: center; gap: 8px; margin-bottom: 12px; }
.color-picker-group input[type='text'] { flex-grow: 1; margin-bottom: 0; }
@media (max-width: 600px) { .container { padding: 8px; } .button-group { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<div class='container'>
<div class='card'>
<h1>打印机状态显示</h1>
<div id='status'>加载中...</div>
<form id='configForm' onsubmit='submitForm(event)'>
)";

const char HTML_FORM_PART1[] PROGMEM = R"(
<label for='uid'>用户ID</label>
<input type='text' id='uid' name='uid' value='%s' required>
<label for='accessToken'>访问令牌</label>
<input type='text' id='accessToken' name='accessToken' maxlength='256' value='%s' required>
<label for='deviceID'>设备序列号</label>
<input type='text' id='deviceID' name='deviceID' value='%s' required>
<label for='brightness'>全局亮度 (0-255)</label>
<input type='number' id='brightness' name='brightness' min='0' max='255' value='%d' required>
<label for='standbyMode'>待机模式</label>
<select id='standbyMode' name='standbyMode'>
<option value='marquee'%s>彩虹跑马灯</option>
<option value='breathing'%s>呼吸灯</option>
</select>
<label for='progressBarColor'>进度条颜色</label>
<div class='color-picker-group'>
<input type='color' id='progressBarColorPicker' name='progressBarColorPicker' value='%s'>
<input type='text' id='progressBarColor' name='progressBarColor' value='%s' pattern='#?([0-9A-Fa-f]{6})' required>
</div>
<label for='standbyBreathingColor'>待机呼吸灯颜色</label>
<div class='color-picker-group'>
<input type='color' id='standbyBreathingColorPicker' name='standbyBreathingColorPicker' value='%s'>
<input type='text' id='standbyBreathingColor' name='standbyBreathingColor' value='%s' pattern='#?([0-9A-Fa-f]{6})' required>
</div>
<label for='progressBarBrightnessRatio'>进度条亮度比例 (0.0-1.0)</label>
<input type='number' id='progressBarBrightnessRatio' name='progressBarBrightnessRatio' min='0' max='1' step='0.1' value='%.1f' required>
<label for='standbyBrightnessRatio'>待机亮度比例 (0.0-1.0)</label>
<input type='number' id='standbyBrightnessRatio' name='standbyBrightnessRatio' min='0' max='1' step='0.1' value='%.1f' required>
<label for='customPushallInterval'>全量包请求间隔 (10-600秒)</label>
<input type='number' id='customPushallInterval' name='customPushallInterval' min='10' max='600' value='%lu' required>
<label><input type='checkbox' id='overlayMarquee' name='overlayMarquee'%s> 在进度条上叠加跑马灯</label>
<button type='submit'>保存配置</button>
</form>
</div>
<div class='card'>
<h2>操作</h2>
<div class='button-group'>
<button onclick='testLed()'>测试 LED</button>
<button onclick='clearCache()'>清除缓存</button>
<button onclick='resetConfig()'>重置配置</button>
<button onclick='restart()'>重启设备</button>
</div>
<div style='margin-top: 16px; display: flex; gap: 10px; align-items: center;'>
<select id='switchModeSelect' name='mode' style='flex-grow: 1; margin-bottom: 0;'>
<option value='progress'>强制进度条</option>
<option value='standby'>强制待机</option>
<option value='none'>自动模式</option>
</select>
<button onclick='switchMode()' style='width: auto; margin-bottom: 0; flex-shrink: 0;'>切换模式</button>
</div>
</div>
<div class='card'>
<h2>日志</h2>
<div class='log-container' id='log'>正在加载日志...</div>
</div>
)";

const char HTML_SCRIPT[] PROGMEM = R"(
<script>
function showMsg(msg, isError = false) {
  alert(msg);
}
function submitForm(event) {
  event.preventDefault();
  const form = document.getElementById('configForm');
  if (!form.checkValidity()) {
    showMsg('请检查表单中的错误项！', true);
    form.reportValidity();
    return;
  }
  const formData = new FormData(form);
  fetch('/config', { method: 'POST', body: formData })
    .then(response => {
      if (response.ok) {
        showMsg('配置保存成功！页面将刷新。');
        setTimeout(() => window.location.reload(), 1000);
      } else {
        return response.text().then(text => { throw new Error('保存失败: ' + (text || response.statusText)); });
      }
    })
    .catch(error => showMsg(error.message, true));
}
function sendPostRequest(url, successMsg, errorMsgBase) {
   fetch(url, { method: 'POST' })
    .then(response => {
      if (response.ok) showMsg(successMsg);
      else return response.text().then(text => { throw new Error(errorMsgBase + ': ' + (text || response.statusText)); });
    })
    .catch(error => showMsg(error.message, true));
}
function testLed() { sendPostRequest('/testLed', '开始测试 LED 灯带！', '测试失败'); }
function clearCache() { sendPostRequest('/clearCache', '缓存已清除！', '清除缓存失败'); }
function resetConfig() {
  if (confirm('确定要重置所有配置吗？设备将重启。')) {
    sendPostRequest('/resetConfig', '配置已重置，设备将重启！', '重置失败');
  }
}
function restart() {
  if (confirm('确定要重启设备吗？')) {
    sendPostRequest('/restart', '设备正在重启！', '重启失败');
  }
}
function switchMode() {
  const mode = document.getElementById('switchModeSelect').value;
  const modeName = {'progress': '强制进度条', 'standby': '强制待机', 'none': '自动模式'}[mode];
  fetch('/switchMode', { method: 'POST', body: new URLSearchParams({ mode }) })
    .then(response => {
      if (response.ok) showMsg('模式已切换到 ' + modeName + '！');
      else return response.text().then(text => { throw new Error('切换模式失败: ' + (text || response.statusText)); });
    })
    .catch(error => showMsg(error.message, true));
}
function fetchLog() {
  fetch('/log')
    .then(response => response.ok ? response.text() : Promise.reject('Failed to load log: ' + response.statusText))
    .then(data => {
      const logDiv = document.getElementById('log');
      logDiv.innerHTML = data;
      logDiv.scrollTop = logDiv.scrollHeight;
    })
    .catch(error => {
      const logDiv = document.getElementById('log');
      if (!logDiv.textContent.includes('日志加载失败')) {
         logDiv.textContent = '日志加载失败: ' + error;
      }
    });
}
function fetchStatus() {
  fetch('/status')
    .then(response => response.ok ? response.json() : Promise.reject('Failed to load status: ' + response.statusText))
    .then(data => {
       document.getElementById('status').textContent = data.status_text || '状态未知';
       document.getElementById('switchModeSelect').value = data.forced_mode || 'none';
    })
    .catch(error => {
       document.getElementById('status').textContent = '状态加载失败: ' + error;
    });
}
function setupColorSync(pickerId, textId) {
  const picker = document.getElementById(pickerId);
  const textInput = document.getElementById(textId);
  picker.addEventListener('input', () => { textInput.value = picker.value; });
  textInput.addEventListener('input', () => {
     if (/^#?([0-9A-Fa-f]{6})$/.test(textInput.value)) {
        let colorVal = textInput.value;
        if (!colorVal.startsWith('#')) {
            colorVal = '#' + colorVal;
        }
        if (colorVal.length === 7) {
           picker.value = colorVal;
        }
     }
  });
  textInput.addEventListener('change', () => {
     let match = textInput.value.match(/^#?([0-9A-Fa-f]{6})$/);
     if (match && !textInput.value.startsWith('#')) {
        textInput.value = '#' + match[1];
     }
  });
}
window.onload = function() {
  fetchStatus();
  fetchLog();
  setInterval(fetchStatus, 5000);
  setInterval(fetchLog, 7000);
  setupColorSync('progressBarColorPicker', 'progressBarColor');
  setupColorSync('standbyBreathingColorPicker', 'standbyBreathingColor');
};
</script>
</body>
</html>
)";

// --- Function Declarations ---
void loadConfig();
void saveConfig();
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void initStaticHtml();
void writeProgmemToFile(File &file, const char* progmem_ptr);

void reconnectMQTT();
void checkWiFiConnection();
void sendPushall();

void mqttCallback(char* topic, byte* payload, unsigned int length);
void writeMqttRxBuffer(const byte* data, unsigned int length);
void writeMqttTxBuffer(const char* data, size_t length);
void processMqttRxBuffer();
void processMqttTxBuffer();

void updateLED();
void updateTestLed();
uint32_t getRainbowColor(float position);
uint32_t colorScale(uint32_t color, float scale);

void handleRoot(AsyncWebServerRequest *request);
void handleConfig(AsyncWebServerRequest *request);
void handleTestLed(AsyncWebServerRequest *request);
void handleLog(AsyncWebServerRequest *request);
void handleStatus(AsyncWebServerRequest *request);
void handleClearCache(AsyncWebServerRequest *request);
void handleResetConfig(AsyncWebServerRequest *request);
void handleSwitchMode(AsyncWebServerRequest *request);
void handleRestart(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);

bool isConfigValid();
bool isPrinting();
String getStateText(State state);
String getForcedModeText(ForcedMode mode);

// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Serial.println(F("\n--- ESP32-C3 Printer LED Status Booting ---"));

  strip.begin();
  strip.setBrightness(globalBrightness);
  strip.clear();
  strip.show();
  Serial.println(F("LED strip initialized."));

  if (!LittleFS.begin()) {
    Serial.println(F("ERROR: Failed to mount LittleFS. Formatting..."));
    if (LittleFS.format()) {
      Serial.println(F("LittleFS formatted successfully. Rebooting..."));
    } else {
      Serial.println(F("ERROR: LittleFS format failed. Halting."));
    }
    delay(1000);
    ESP.restart();
    while (1) { yield(); }
  }
  Serial.print(F("LittleFS mounted. Total: ")); Serial.print(LittleFS.totalBytes());
  Serial.print(F(" Used: ")); Serial.print(LittleFS.usedBytes());
  Serial.print(F(" Free: ")); Serial.println(LittleFS.totalBytes() - LittleFS.usedBytes());

  if (LittleFS.exists(MQTT_RX_BUFFER_FILE)) LittleFS.remove(MQTT_RX_BUFFER_FILE);
  if (LittleFS.exists(MQTT_TX_BUFFER_FILE)) LittleFS.remove(MQTT_TX_BUFFER_FILE);
  Serial.println(F("Cleared MQTT buffer files."));

  loadConfig();
  initStaticHtml();

  WiFi.mode(WIFI_STA);
  wm.setHostname("PrinterLED");
  WiFiManagerParameter custom_uid("uid", "用户ID", uid, 32);
  WiFiManagerParameter custom_access_token("accessToken", "访问令牌", accessToken, 256, "type='password'");
  WiFiManagerParameter custom_device_id("deviceID", "设备序列号", deviceID, 32);
  if (!isConfigValid()) {
    wm.addParameter(&custom_uid);
    wm.addParameter(&custom_access_token);
    wm.addParameter(&custom_device_id);
    Serial.println(F("MQTT config missing, adding parameters to WiFiManager."));
  }
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);

  currentState = CONNECTING_WIFI;
  Serial.println(F("Attempting WiFi connection..."));
  if (!wm.autoConnect("BambuLED-Setup")) {
    Serial.println(F("Failed to connect to WiFi and timeout occurred. Entering AP mode."));
  } else {
    Serial.print(F("WiFi connected! IP Address: "));
    Serial.println(WiFi.localIP());
    currentState = CONNECTED_WIFI;

    if (!isConfigValid()) {
      strncpy(uid, custom_uid.getValue(), sizeof(uid) - 1);
      uid[sizeof(uid) - 1] = '\0';
      strncpy(accessToken, custom_access_token.getValue(), sizeof(accessToken) - 1);
      accessToken[sizeof(accessToken) - 1] = '\0';
      strncpy(deviceID, custom_device_id.getValue(), sizeof(deviceID) - 1);
      deviceID[sizeof(deviceID) - 1] = '\0';
      if (strlen(uid) > 0 && strlen(accessToken) > 0 && strlen(deviceID) > 0) {
        saveConfig();
        Serial.println(F("Saved MQTT config from WiFiManager."));
        initStaticHtml();
      } else {
        Serial.println(F("WARNING: Incomplete MQTT config entered via WiFiManager, not saving."));
      }
    }

    Serial.println(F("Syncing time via NTP..."));
    configTime(8 * 3600, 0, "pool.ntp.org", "time.windows.com");
    time_t now = time(nullptr);
    int retries = 0;
    while (now < 1000000000 && retries < 10) {
      delay(1000);
      now = time(nullptr);
      Serial.print(".");
      retries++;
    }
    if (now >= 1000000000) {
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);
      Serial.print(F("\nTime synchronized: "));
      Serial.print(asctime(&timeinfo));
    } else {
      Serial.println(F("\nNTP Sync failed."));
    }
  }

  if (isConfigValid()) {
    espClient.setInsecure();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);
    Serial.println(F("MQTT client configured."));
  } else {
    Serial.println(F("WARNING: MQTT configuration is incomplete. Cannot connect to MQTT."));
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/testLed", HTTP_POST, handleTestLed);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/clearCache", HTTP_POST, handleClearCache);
  server.on("/resetConfig", HTTP_POST, handleResetConfig);
  server.on("/switchMode", HTTP_POST, handleSwitchMode);
  server.on("/restart", HTTP_POST, handleRestart);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("HTTP server started on port 80"));

  esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  Serial.println(F("--- Boot sequence complete ---"));
}

// --- Main Loop ---
void loop() {
  esp_task_wdt_reset();

  unsigned long currentMillis = millis();

  if (currentMillis - lastOperationCheck > OPERATION_CHECK_INTERVAL) {
    lastOperationCheck = currentMillis;
    Serial.print(F("[Health] State: ")); Serial.print(getStateText(currentState));
    Serial.print(F(", Heap: ")); Serial.print(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.print(F(", Forced: ")); Serial.println(getForcedModeText(forcedMode));
  }

  checkWiFiConnection();

  if (currentState != AP_MODE && WiFi.status() == WL_CONNECTED && isConfigValid()) {
    if (!mqttClient.connected()) {
      if (currentState != CONNECTING_PRINTER) {
        currentState = CONNECTING_PRINTER;
        Serial.println(F("MQTT disconnected. Attempting reconnect..."));
      }
      reconnectMQTT();
    } else {
      mqttClient.loop();
      processMqttTxBuffer();
    }
  }

  processMqttRxBuffer();

  if (currentState >= CONNECTED_PRINTER && mqttClient.connected() && currentMillis - lastPushallTime > (customPushallInterval * 1000)) {
    sendPushall();
    lastPushallTime = currentMillis;
  } else if (currentState >= CONNECTED_PRINTER && currentMillis - lastPushallTime > (customPushallInterval * 1000)) {
    pendingPushall = true;
  }

  if (pendingPushall && mqttClient.connected()) {
    sendPushall();
    pendingPushall = false;
    lastPushallTime = currentMillis;
  }

  if (currentMillis - lastMqttMessageTime > MQTT_TIMEOUT) {
    Serial.println(F("MQTT 超时，切换到 ERROR 状态"));
    currentState = ERROR;
  }

  updateLED();
  updateTestLed();

  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 20000) {
    Serial.println(F("内存低，重启..."));
    ESP.restart();
  }

  if (currentState != lastState) {
    Serial.print(F("[State Change] From ")); Serial.print(getStateText(lastState));
    Serial.print(F(" to ")); Serial.println(getStateText(currentState));
    lastState = currentState;
  }

  yield();
}

// --- Configuration Functions ---
void loadConfig() {
  Serial.println(F("Loading configuration from /config.json..."));
  if (LittleFS.exists("/config.json")) {
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, configFile);
      configFile.close();

      if (!error) {
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
        Serial.println(F("Configuration loaded successfully."));
      } else {
        Serial.print(F("ERROR: Failed to parse config file: "));
        Serial.println(error.c_str());
      }
    } else {
      Serial.println(F("ERROR: Failed to open config file for reading."));
    }
  } else {
    Serial.println(F("Config file not found. Using default values."));
  }
}

void saveConfig() {
  Serial.println(F("Saving configuration to /config.json..."));
  DynamicJsonDocument doc(1024);

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
  if (configFile) {
    if (serializeJson(doc, configFile) > 0) {
      Serial.println(F("Configuration saved successfully."));
    } else {
      Serial.println(F("ERROR: Failed to write to config file."));
    }
    configFile.close();
  } else {
    Serial.println(F("ERROR: Failed to open config file for writing."));
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.print(F("Entered AP Mode. SSID: "));
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print(F("IP Address: "));
  Serial.println(WiFi.softAPIP());
  currentState = AP_MODE;
  apClientConnected = false;
  strip.clear();
  strip.setPixelColor(0, strip.Color(0, 0, 255));
  strip.show();
}

void saveConfigCallback() {
  Serial.println(F("WiFiManager saved WiFi credentials."));
}

void writeProgmemToFile(File &file, const char* progmem_ptr) {
  const size_t chunkSize = 512;
  char buffer[chunkSize];
  size_t totalLen = strlen_P(progmem_ptr);
  for (size_t i = 0; i < totalLen; i += chunkSize) {
    size_t len = min(chunkSize, totalLen - i);
    if (len > 0) {
      memcpy_P(buffer, progmem_ptr + i, len);
      if (file.write(reinterpret_cast<const uint8_t*>(buffer), len) != len) {
        Serial.println(F("ERROR: Failed to write PROGMEM chunk to file."));
        break;
      }
    }
    esp_task_wdt_reset();
    yield();
  }
}

void initStaticHtml() {
  Serial.println(F("Generating /index.html..."));
  File file = LittleFS.open("/index.html", "w");
  if (!file) {
    Serial.println(F("ERROR: Failed to open /index.html for writing."));
    return;
  }

  writeProgmemToFile(file, HTML_HEAD);

  char tempBuffer[300];

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='uid'>用户ID</label><input type='text' id='uid' name='uid' value='%s' required>"), uid);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='accessToken'>访问令牌</label><input type='text' id='accessToken' name='accessToken' maxlength='256' value='%s' required>"), accessToken);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='deviceID'>设备序列号</label><input type='text' id='deviceID' name='deviceID' value='%s' required>"), deviceID);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='brightness'>全局亮度 (0-255)</label><input type='number' id='brightness' name='brightness' min='0' max='255' value='%d' required>"), globalBrightness);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='standbyMode'>待机模式</label><select id='standbyMode' name='standbyMode'><option value='marquee'%s>彩虹跑马灯</option><option value='breathing'%s>呼吸灯</option></select>"),
             (strcmp(standbyMode, "marquee") == 0 ? PSTR(" selected") : PSTR("")),
             (strcmp(standbyMode, "breathing") == 0 ? PSTR(" selected") : PSTR("")));
  file.print(tempBuffer);

  char progressBarColorHex[8];
  snprintf(progressBarColorHex, sizeof(progressBarColorHex), "#%06lX", progressBarColor);
  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='progressBarColor'>进度条颜色</label><div class='color-picker-group'><input type='color' id='progressBarColorPicker' name='progressBarColorPicker' value='%s'><input type='text' id='progressBarColor' name='progressBarColor' value='%s' pattern='#?([0-9A-Fa-f]{6})' required></div>"), progressBarColorHex, progressBarColorHex);
  file.print(tempBuffer);

  char standbyBreathingColorHex[8];
  snprintf(standbyBreathingColorHex, sizeof(standbyBreathingColorHex), "#%06lX", standbyBreathingColor);
  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='standbyBreathingColor'>待机呼吸灯颜色</label><div class='color-picker-group'><input type='color' id='standbyBreathingColorPicker' name='standbyBreathingColorPicker' value='%s'><input type='text' id='standbyBreathingColor' name='standbyBreathingColor' value='%s' pattern='#?([0-9A-Fa-f]{6})' required></div>"), standbyBreathingColorHex, standbyBreathingColorHex);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='progressBarBrightnessRatio'>进度条亮度比例 (0.0-1.0)</label><input type='number' id='progressBarBrightnessRatio' name='progressBarBrightnessRatio' min='0' max='1' step='0.1' value='%.1f' required>"), progressBarBrightnessRatio);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='standbyBrightnessRatio'>待机亮度比例 (0.0-1.0)</label><input type='number' id='standbyBrightnessRatio' name='standbyBrightnessRatio' min='0' max='1' step='0.1' value='%.1f' required>"), standbyBrightnessRatio);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label for='customPushallInterval'>全量包请求间隔 (10-600秒)</label><input type='number' id='customPushallInterval' name='customPushallInterval' min='10' max='600' value='%lu' required>"), customPushallInterval);
  file.print(tempBuffer);

  snprintf_P(tempBuffer, sizeof(tempBuffer), PSTR("<label><input type='checkbox' id='overlayMarquee' name='overlayMarquee'%s> 在进度条上叠加跑马灯</label>"), (overlayMarquee ? PSTR(" checked") : PSTR("")));
  file.print(tempBuffer);

  writeProgmemToFile(file, HTML_FORM_PART1 + strlen_P(HTML_FORM_PART1) - strlen_P("</div></div></div>"));
  writeProgmemToFile(file, HTML_SCRIPT);

  file.close();
  Serial.println(F("Generated /index.html"));
}

// --- Networking Functions ---
void checkWiFiConnection() {
  unsigned long currentMillis = millis();
  if (currentState != AP_MODE && currentMillis - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("WiFi disconnected. Trying to reconnect..."));
      currentState = CONNECTING_WIFI;
      mqttClient.disconnect();
    }
  }
}

void reconnectMQTT() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastReconnectAttempt < reconnectDelay) {
    return;
  }
  lastReconnectAttempt = currentMillis;

  Serial.print(F("Attempting MQTT connection... ClientID: "));
  uint64_t mac = ESP.getEfuseMac();
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  String clientID = "BambuLED-" + String(macStr);
  Serial.print(clientID);
  Serial.print(F(" User: "));
  Serial.println(uid);

  if (mqttClient.connect(clientID.c_str(), uid, accessToken)) {
    Serial.println(F("MQTT connected successfully!"));
    currentState = CONNECTED_PRINTER;

    char topicSubBuffer[64];
    strcpy_P(topicSubBuffer, MQTT_TOPIC_SUB_TEMPLATE);
    String topicSub = String(topicSubBuffer);
    topicSub.replace("{DEVICE_ID}", deviceID);

    if (mqttClient.subscribe(topicSub.c_str())) {
      Serial.print(F("Subscribed to: "));
      Serial.println(topicSub);
    } else {
      Serial.println(F("ERROR: Failed to subscribe to report topic."));
    }

    sendPushall();
    lastPushallTime = currentMillis;
    reconnectDelay = 1000;
  } else {
    Serial.print(F("MQTT connection failed, rc="));
    Serial.print(mqttClient.state());
    Serial.print(F(". Retrying in "));
    Serial.print(reconnectDelay / 1000);
    Serial.println(F(" seconds."));
    reconnectDelay = min(reconnectDelay * 2, MAX_RECONNECT_DELAY);
    currentState = CONNECTING_PRINTER;
  }
}

void sendPushall() {
  DynamicJsonDocument pushall_request(256);
  JsonObject pushing = pushall_request.createNestedObject("pushing");
  pushing["sequence_id"] = String(millis());
  pushing["command"] = "pushall";
  pushing["version"] = 1;
  pushing["push_target"] = 1;

  char payload[128];
  size_t length = serializeJson(pushall_request, payload, sizeof(payload));

  if (length > 0) {
    writeMqttTxBuffer(payload, length);
    Serial.println(F("Buffered pushall request."));
    if (mqttClient.connected()) {
      processMqttTxBuffer();
    }
  } else {
    Serial.println(F("ERROR: Failed to serialize pushall request."));
  }
}

// --- MQTT Handling Functions ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  lastMqttMessageTime = millis();
  Serial.print(F("MQTT Message Received ["));
  Serial.print(topic);
  Serial.print(F("] Length: "));
  Serial.println(length);

  writeMqttRxBuffer(payload, length);
}

void writeMqttRxBuffer(const byte* data, unsigned int length) {
  File file = LittleFS.open(MQTT_RX_BUFFER_FILE, "a");
  if (!file) {
    Serial.println(F("ERROR: Failed to open MQTT RX buffer file for writing."));
    return;
  }
  file.write(reinterpret_cast<const uint8_t*>(&length), sizeof(length));
  size_t bytesWritten = file.write(data, length);
  file.close();

  if (bytesWritten != length) {
    Serial.print(F("ERROR: Failed to write complete message to RX buffer. Written: "));
    Serial.println(bytesWritten);
  }
}

void writeMqttTxBuffer(const char* data, size_t length) {
  File file = LittleFS.open(MQTT_TX_BUFFER_FILE, "a");
  if (!file) {
    Serial.println(F("ERROR: Failed to open MQTT TX buffer file for writing."));
    return;
  }
  file.write(reinterpret_cast<const uint8_t*>(&length), sizeof(length));
  size_t bytesWritten = file.write(reinterpret_cast<const uint8_t*>(data), length);
  file.close();

  if (bytesWritten != length) {
    Serial.print(F("ERROR: Failed to write complete message to TX buffer. Written: "));
    Serial.println(bytesWritten);
  }
}

void processMqttRxBuffer() {
  if (!LittleFS.exists(MQTT_RX_BUFFER_FILE)) {
    return;
  }

  size_t offset = 0;
  char buffer[MQTT_BUFFER_BLOCK_SIZE + 1];
  size_t bytesRead;
  bool processedAny = false;
  size_t processedOffset = 0;
  DynamicJsonDocument doc(4096);

  while (true) {
    File file = LittleFS.open(MQTT_RX_BUFFER_FILE, "r");
    if (!file || offset >= file.size()) {
      if (file) file.close();
      break;
    }

    if (!file.seek(offset)) {
      Serial.println(F("ERROR: Failed to seek in RX buffer file."));
      file.close();
      break;
    }

    if (file.readBytes(reinterpret_cast<char*>(&bytesRead), sizeof(bytesRead)) != sizeof(bytesRead)) {
      file.close();
      break;
    }

    if (bytesRead == 0 || bytesRead > MQTT_BUFFER_BLOCK_SIZE) {
      offset += sizeof(bytesRead) + bytesRead;
      file.close();
      continue;
    }

    size_t actualRead = file.readBytes(buffer, bytesRead);
    size_t expectedOffsetEnd = offset + sizeof(bytesRead) + actualRead;
    file.close();

    if (actualRead != bytesRead) {
      offset = expectedOffsetEnd;
      continue;
    }

    buffer[bytesRead] = '\0';
    doc.clear();
    DeserializationError error = deserializeJson(doc, buffer, bytesRead);

    if (error) {
      Serial.print(F("ERROR: Failed to parse JSON from RX buffer: "));
      Serial.println(error.c_str());
      processedOffset = offset = expectedOffsetEnd;
      processedAny = true;
      continue;
    }

    if (doc.containsKey("print")) {
      JsonObject printData = doc["print"];
      for (JsonPair kv : printData) {
        printerState[kv.key().c_str()] = kv.value();
      }

      gcodeState = printerState["gcode_state"] | "UNKNOWN";
      printPercent = printerState["mc_percent"] | 0;
      remainingTime = printerState["mc_remaining_time"] | 0;
      layerNum = printerState["layer_num"] | 0;

      if (forcedMode == NONE) {
        State newState = currentState;
        if (gcodeState == "RUNNING") newState = PRINTING;
        else if (gcodeState == "FAILED" || gcodeState == "STOP") newState = ERROR;
        else if (gcodeState == "FINISH" || gcodeState == "IDLE" || gcodeState == "PAUSE") newState = CONNECTED_PRINTER;
        else if (currentState < CONNECTED_PRINTER) newState = currentState;
        else newState = CONNECTED_PRINTER;

        if (newState != currentState) currentState = newState;
      }
    }

    processedOffset = offset = expectedOffsetEnd;
    processedAny = true;
    yield();
  }

  if (processedAny && processedOffset > 0) {
    File readFile = LittleFS.open(MQTT_RX_BUFFER_FILE, "r");
    if (!readFile) {
      Serial.println(F("ERROR: Cannot open RX buffer for cleanup read."));
      return;
    }
    size_t totalSize = readFile.size();
    readFile.close();

    if (processedOffset >= totalSize) {
      if (LittleFS.remove(MQTT_RX_BUFFER_FILE)) {
        // Serial.println(F("Cleaned up processed RX buffer file."));
      } else {
        Serial.println(F("ERROR: Failed to remove processed RX buffer file."));
      }
    } else {
      Serial.println(F("Partially processed RX buffer, rewriting remaining data..."));
      const char* tempFileName = "/mqtt_rx_temp.json";
      File sourceFile = LittleFS.open(MQTT_RX_BUFFER_FILE, "r");
      File tempFile = LittleFS.open(tempFileName, "w");

      if (!tempFile || !sourceFile) {
        Serial.println(F("ERROR: Failed to open files for RX buffer cleanup rewrite."));
        if (tempFile) tempFile.close();
        if (sourceFile) sourceFile.close();
        if (LittleFS.exists(tempFileName)) LittleFS.remove(tempFileName);
        return;
      }

      if (!sourceFile.seek(processedOffset)) {
        Serial.println(F("ERROR: Failed seek during RX buffer cleanup rewrite."));
        tempFile.close();
        sourceFile.close();
        LittleFS.remove(tempFileName);
        return;
      }

      char copyBuf[256];
      size_t bytesCopied = 0;
      while (sourceFile.available()) {
        size_t bytesToRead = sourceFile.readBytes(copyBuf, sizeof(copyBuf));
        if (bytesToRead > 0) {
          size_t bytesWritten = tempFile.write(reinterpret_cast<const uint8_t*>(copyBuf), bytesToRead);
          if (bytesWritten != bytesToRead) {
            Serial.println(F("ERROR: Write error during RX buffer cleanup rewrite."));
            tempFile.close();
            sourceFile.close();
            LittleFS.remove(tempFileName);
            return;
          }
          bytesCopied += bytesWritten;
        }
        yield();
      }

      tempFile.close();
      sourceFile.close();

      if (LittleFS.remove(MQTT_RX_BUFFER_FILE)) {
        if (LittleFS.rename(tempFileName, MQTT_RX_BUFFER_FILE)) {
          Serial.print(F("Rewrote RX buffer file, remaining bytes: "));
          Serial.println(bytesCopied);
        } else {
          Serial.println(F("ERROR: Failed to rename temp RX buffer file."));
          LittleFS.remove(tempFileName);
        }
      } else {
        Serial.println(F("ERROR: Failed to remove original RX buffer file during cleanup."));
        LittleFS.remove(tempFileName);
      }
    }
  }
}

void processMqttTxBuffer() {
  if (!mqttClient.connected() || !LittleFS.exists(MQTT_TX_BUFFER_FILE)) {
    return;
  }

  size_t offset = 0;
  char buffer[MQTT_BUFFER_BLOCK_SIZE + 1];
  size_t bytesRead;
  bool sentAny = false;
  size_t processedOffset = 0;

  char topicPubBuffer[64];
  strcpy_P(topicPubBuffer, MQTT_TOPIC_PUB_TEMPLATE);
  String topicPub = String(topicPubBuffer);
  topicPub.replace("{DEVICE_ID}", deviceID);

  while (true) {
    File file = LittleFS.open(MQTT_TX_BUFFER_FILE, "r");
    if (!file || offset >= file.size()) {
      if (file) file.close();
      break;
    }

    if (!file.seek(offset)) {
      Serial.println(F("ERROR: Failed to seek in TX buffer file."));
      file.close();
      break;
    }

    if (file.readBytes(reinterpret_cast<char*>(&bytesRead), sizeof(bytesRead)) != sizeof(bytesRead)) {
      file.close();
      break;
    }

    if (bytesRead == 0 || bytesRead > MQTT_BUFFER_BLOCK_SIZE) {
      offset += sizeof(bytesRead) + bytesRead;
      file.close();
      continue;
    }

    size_t actualRead = file.readBytes(buffer, bytesRead);
    size_t expectedOffsetEnd = offset + sizeof(bytesRead) + actualRead;
    file.close();

    if (actualRead != bytesRead) {
      offset = expectedOffsetEnd;
      continue;
    }

    buffer[bytesRead] = '\0';
    if (mqttClient.publish(topicPub.c_str(), reinterpret_cast<const uint8_t*>(buffer), bytesRead)) {
      processedOffset = offset = expectedOffsetEnd;
      sentAny = true;
    } else {
      Serial.println(F("ERROR: Failed to publish message from TX buffer."));
      break;
    }
    yield();
  }

  if (sentAny && processedOffset > 0) {
    File readFile = LittleFS.open(MQTT_TX_BUFFER_FILE, "r");
    if (!readFile) {
      Serial.println(F("ERROR: Cannot open TX buffer for cleanup read."));
      return;
    }
    size_t totalSize = readFile.size();
    readFile.close();

    if (processedOffset >= totalSize) {
      if (LittleFS.remove(MQTT_TX_BUFFER_FILE)) {
        // Serial.println(F("Cleaned up processed TX buffer file."));
      } else {
        Serial.println(F("ERROR: Failed to remove processed TX buffer file."));
      }
    } else {
      Serial.println(F("Partially processed TX buffer, rewriting remaining data..."));
      const char* tempFileName = "/mqtt_tx_temp.json";
      File sourceFile = LittleFS.open(MQTT_TX_BUFFER_FILE, "r");
      File tempFile = LittleFS.open(tempFileName, "w");

      if (!tempFile || !sourceFile) {
        Serial.println(F("ERROR: Failed to open files for TX buffer cleanup rewrite."));
        if (tempFile) tempFile.close();
        if (sourceFile) sourceFile.close();
        if (LittleFS.exists(tempFileName)) LittleFS.remove(tempFileName);
        return;
      }

      if (!sourceFile.seek(processedOffset)) {
        Serial.println(F("ERROR: Failed seek during TX buffer cleanup rewrite."));
        tempFile.close();
        sourceFile.close();
        LittleFS.remove(tempFileName);
        return;
      }

      char copyBuf[256];
      size_t bytesCopied = 0;
      while (sourceFile.available()) {
        size_t bytesToRead = sourceFile.readBytes(copyBuf, sizeof(copyBuf));
        if (bytesToRead > 0) {
          size_t bytesWritten = tempFile.write(reinterpret_cast<const uint8_t*>(copyBuf), bytesToRead);
          if (bytesWritten != bytesToRead) {
            Serial.println(F("ERROR: Write error during TX buffer cleanup rewrite."));
            tempFile.close();
            sourceFile.close();
            LittleFS.remove(tempFileName);
            return;
          }
          bytesCopied += bytesWritten;
        }
        yield();
      }

      tempFile.close();
      sourceFile.close();

      if (LittleFS.remove(MQTT_TX_BUFFER_FILE)) {
        if (LittleFS.rename(tempFileName, MQTT_TX_BUFFER_FILE)) {
          Serial.print(F("Rewrote TX buffer file, remaining bytes: "));
          Serial.println(bytesCopied);
        } else {
          Serial.println(F("ERROR: Failed to rename temp TX buffer file."));
          LittleFS.remove(tempFileName);
        }
      } else {
        Serial.println(F("ERROR: Failed to remove original TX buffer file during cleanup."));
        LittleFS.remove(tempFileName);
      }
    }
  }
}

// --- LED Control Functions ---
uint32_t colorScale(uint32_t color, float scale) {
  scale = constrain(scale, 0.0f, 1.0f);
  uint8_t r = (uint8_t)(((color >> 16) & 0xFF) * scale);
  uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * scale);
  uint8_t b = (uint8_t)((color & 0xFF) * scale);
  return strip.Color(r, g, b);
}

uint32_t getRainbowColor(float position) {
  int hue = (int)(position * 255);
  int section = hue / 43;
  int remainder = hue - (section * 43);
  int intensity = remainder * 6;
  switch (section) {
    case 0: return strip.Color(255, intensity, 0);
    case 1: return strip.Color(255 - intensity, 255, 0);
    case 2: return strip.Color(0, 255, intensity);
    case 3: return strip.Color(0, 255 - intensity, 255);
    case 4: return strip.Color(intensity, 0, 255);
    default: return strip.Color(255, 0, 255 - intensity);
  }
}

void updateLED() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastLedUpdate < LED_UPDATE_INTERVAL || testingLed) {
    return;
  }
  lastLedUpdate = currentMillis;

  strip.clear();

  float currentBrightnessRatio = 1.0;
  uint32_t currentBaseColor = strip.Color(0, 0, 0);

  State displayState = currentState;
  if (forcedMode == PROGRESS) displayState = PRINTING;
  else if (forcedMode == STANDBY) displayState = CONNECTED_PRINTER;

  strip.setBrightness(constrain(globalBrightness * currentBrightnessRatio, 0, 255));

  switch (displayState) {
    case AP_MODE:
      apClientConnected = WiFi.softAPgetStationNum() > 0;
      currentBaseColor = apClientConnected ? strip.Color(0, 255, 0) : strip.Color(0, 0, 255);
      strip.setPixelColor(0, colorScale(currentBaseColor, (sin(currentMillis / 300.0 * PI) + 1) / 2.0));
      currentBrightnessRatio = 1.0;
      break;

    case CONNECTING_WIFI:
    case CONNECTING_PRINTER:
      strip.setPixelColor(0, (currentMillis / 500) % 2 ? strip.Color(255, 0, 0) : strip.Color(0, 0, 255));
      currentBrightnessRatio = 1.0;
      break;

    case CONNECTED_WIFI:
      strip.setPixelColor(0, strip.Color(0, 0, 255));
      strip.setPixelColor(1, (currentMillis / 500) % 2 ? strip.Color(255, 0, 0) : 0);
      currentBrightnessRatio = 1.0;
      break;

    case PRINTING:
      currentBrightnessRatio = progressBarBrightnessRatio;
      currentBaseColor = progressBarColor;
      {
        float pixelsToLight = (float)printPercent * LED_COUNT / 100.0;
        int fullPixels = (int)pixelsToLight;
        float partialPixelBrightness = pixelsToLight - fullPixels;

        fullPixels = constrain(fullPixels, 0, LED_COUNT);

        for (int i = 0; i < fullPixels; i++) {
          strip.setPixelColor(i, currentBaseColor);
        }

        if (fullPixels < LED_COUNT && partialPixelBrightness > 0.01) {
          strip.setPixelColor(fullPixels, colorScale(currentBaseColor, partialPixelBrightness));
        }

        if (overlayMarquee) {
          int currentMarqueePos = marqueePosition;
          for (int i = 0; i < LED_COUNT; i++) {
            if (i < fullPixels || (i == fullPixels && partialPixelBrightness > 0.01) || strip.getPixelColor(i) == 0) {
              float rainbowPos = ((float)(i - currentMarqueePos + LED_COUNT * 2) / (LED_COUNT * 2.0));
              rainbowPos -= (int)rainbowPos;
              uint32_t rainbowColor = getRainbowColor(rainbowPos);
              uint32_t existingColor = strip.getPixelColor(i);

              uint8_t r1 = (existingColor >> 16) & 0xFF;
              uint8_t g1 = (existingColor >> 8) & 0xFF;
              uint8_t b1 = existingColor & 0xFF;
              uint8_t r2 = (rainbowColor >> 16) & 0xFF;
              uint8_t g2 = (rainbowColor >> 8) & 0xFF;
              uint8_t b2 = rainbowColor & 0xFF;

              float blendFactor = (existingColor == 0) ? 0.3 : 0.5;
              strip.setPixelColor(i, strip.Color(
                (uint8_t)(r1 * (1.0 - blendFactor) + r2 * blendFactor),
                (uint8_t)(g1 * (1.0 - blendFactor) + g2 * blendFactor),
                (uint8_t)(b1 * (1.0 - blendFactor) + b2 * blendFactor)
              ));
            }
          }
        }
      }
      break;

    case CONNECTED_PRINTER:
      currentBrightnessRatio = standbyBrightnessRatio;
      if (strcmp(standbyMode, "marquee") == 0) {
        int currentMarqueePos = marqueePosition;
        for (int i = 0; i < LED_COUNT; i++) {
          float pos = ((float)(i - currentMarqueePos + LED_COUNT * 2) / (LED_COUNT * 2.0));
          pos -= (int)pos;
          strip.setPixelColor(i, getRainbowColor(pos));
        }
      } else if (strcmp(standbyMode, "breathing") == 0) {
        currentBaseColor = standbyBreathingColor;
        float breath = (sin(currentMillis / 1000.0 * PI) + 1.0) / 2.0;
        uint32_t breathColor = colorScale(currentBaseColor, breath);
        for (int i = 0; i < LED_COUNT; i++) {
          strip.setPixelColor(i, breathColor);
        }
      }
      break;

    case ERROR:
      currentBaseColor = strip.Color(255, 0, 0);
      if ((currentMillis / 500) % 2) {
        for (int i = 0; i < LED_COUNT; i++) {
          strip.setPixelColor(i, currentBaseColor);
        }
      }
      currentBrightnessRatio = 1.0;
      break;

    default:
      strip.setPixelColor(0, (currentMillis / 250) % 2 ? strip.Color(255, 0, 0) : strip.Color(0, 0, 255));
      currentBrightnessRatio = 1.0;
      Serial.println(F("WARNING: Unknown state in updateLED, using default flashing pattern."));
      break;
  }

  marqueePosition = (marqueePosition + 1) % (LED_COUNT * 2);
  strip.show();
}

void updateTestLed() {
  unsigned long currentMillis = millis();
  if (!testingLed || currentMillis - lastTestLedUpdate < TEST_LED_INTERVAL) {
    return;
  }
  lastTestLedUpdate = currentMillis;

  strip.clear();
  if (testLedIndex >= LED_COUNT) {
    testingLed = false;
    testLedIndex = 0;
    Serial.println(F("LED test completed."));
    return;
  }

  strip.setPixelColor(testLedIndex, strip.Color(255, 255, 255));
  strip.show();
  testLedIndex++;
}

// --- Utility Functions ---
String getStateText(State state) {
  switch (state) {
    case AP_MODE: return F("AP_MODE");
    case CONNECTING_WIFI: return F("CONNECTING_WIFI");
    case CONNECTED_WIFI: return F("CONNECTED_WIFI");
    case CONNECTING_PRINTER: return F("CONNECTING_PRINTER");
    case CONNECTED_PRINTER: return F("CONNECTED_PRINTER");
    case PRINTING: return F("PRINTING");
    case ERROR: return F("ERROR");
    default: return F("UNKNOWN");
  }
}

String getForcedModeText(ForcedMode mode) {
  switch (mode) {
    case NONE: return F("NONE");
    case PROGRESS: return F("PROGRESS");
    case STANDBY: return F("STANDBY");
    default: return F("UNKNOWN");
  }
}

bool isConfigValid() {
  return strlen(uid) > 0 && strlen(accessToken) > 0 && strlen(deviceID) > 0;
}

bool isPrinting() {
  return currentState == PRINTING;
}

// --- Web Server Handlers ---
void handleRoot(AsyncWebServerRequest *request) {
  if (LittleFS.exists("/index.html")) {
    request->send(LittleFS, "/index.html", "text/html");
  } else {
    request->send(500, "text/plain", "Internal Server Error: /index.html not found.");
  }
}

void handleConfig(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }

  String newUid = request->hasParam("uid", true) ? request->getParam("uid", true)->value() : "";
  String newAccessToken = request->hasParam("accessToken", true) ? request->getParam("accessToken", true)->value() : "";
  String newDeviceID = request->hasParam("deviceID", true) ? request->getParam("deviceID", true)->value() : "";
  int newBrightness = request->hasParam("brightness", true) ? request->getParam("brightness", true)->value().toInt() : globalBrightness;
  String newStandbyMode = request->hasParam("standbyMode", true) ? request->getParam("standbyMode", true)->value() : "marquee";
  bool newOverlayMarquee = request->hasParam("overlayMarquee", true);
  String progressBarColorHex = request->hasParam("progressBarColor", true) ? request->getParam("progressBarColor", true)->value() : "#00FF00";
  String standbyBreathingColorHex = request->hasParam("standbyBreathingColor", true) ? request->getParam("standbyBreathingColor", true)->value() : "#0000FF";
  float newProgressBarBrightnessRatio = request->hasParam("progressBarBrightnessRatio", true) ? request->getParam("progressBarBrightnessRatio", true)->value().toFloat() : 1.0;
  float newStandbyBrightnessRatio = request->hasParam("standbyBrightnessRatio", true) ? request->getParam("standbyBrightnessRatio", true)->value().toFloat() : 1.0;
  unsigned long newCustomPushallInterval = request->hasParam("customPushallInterval", true) ? request->getParam("customPushallInterval", true)->value().toInt() : 30;

  if (newUid.length() > 0 && newUid.length() < sizeof(uid) &&
      newAccessToken.length() > 0 && newAccessToken.length() < sizeof(accessToken) &&
      newDeviceID.length() > 0 && newDeviceID.length() < sizeof(deviceID) &&
      newBrightness >= 0 && newBrightness <= 255 &&
      (newStandbyMode == "marquee" || newStandbyMode == "breathing") &&
      progressBarColorHex.length() == 7 && progressBarColorHex.startsWith("#") &&
      standbyBreathingColorHex.length() == 7 && standbyBreathingColorHex.startsWith("#") &&
      newProgressBarBrightnessRatio >= 0.0 && newProgressBarBrightnessRatio <= 1.0 &&
      newStandbyBrightnessRatio >= 0.0 && newStandbyBrightnessRatio <= 1.0 &&
      newCustomPushallInterval >= 10 && newCustomPushallInterval <= 600) {

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

    progressBarColorHex.remove(0, 1);
    progressBarColor = strtoul(progressBarColorHex.c_str(), nullptr, 16);
    standbyBreathingColorHex.remove(0, 1);
    standbyBreathingColor = strtoul(standbyBreathingColorHex.c_str(), nullptr, 16);

    progressBarBrightnessRatio = newProgressBarBrightnessRatio;
    standbyBrightnessRatio = newStandbyBrightnessRatio;
    customPushallInterval = newCustomPushallInterval;

    strip.setBrightness(globalBrightness);
    saveConfig();
    initStaticHtml();

    if (isConfigValid() && WiFi.status() == WL_CONNECTED) {
      mqttClient.disconnect();
      espClient.setInsecure();
      mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
      mqttClient.setCallback(mqttCallback);
      mqttClient.setBufferSize(512);
      reconnectMQTT();
    }

    request->send(200, "text/plain", "Configuration updated successfully.");
  } else {
    request->send(400, "text/plain", "Invalid configuration parameters.");
  }
}

void handleTestLed(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  testingLed = true;
  testLedIndex = 0;
  Serial.println(F("Starting LED test..."));
  request->send(200, "text/plain", "LED test started.");
}

void handleLog(AsyncWebServerRequest *request) {
  String log = Serial.readString();
  if (log.length() == 0) {
    log = "No new logs available.\n";
  }
  request->send(200, "text/plain", log);
}

void handleStatus(AsyncWebServerRequest *request) {
  DynamicJsonDocument doc(512);
  doc["status"] = (int)currentState;
  doc["status_text"] = getStateText(currentState);
  doc["forced_mode"] = getForcedModeText(forcedMode);
  doc["print_percent"] = printPercent;
  doc["gcode_state"] = gcodeState;
  doc["remaining_time"] = remainingTime;
  doc["layer_num"] = layerNum;
  doc["heap_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void handleClearCache(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  if (LittleFS.exists(MQTT_RX_BUFFER_FILE)) LittleFS.remove(MQTT_RX_BUFFER_FILE);
  if (LittleFS.exists(MQTT_TX_BUFFER_FILE)) LittleFS.remove(MQTT_TX_BUFFER_FILE);
  Serial.println(F("Cleared MQTT buffer cache."));
  request->send(200, "text/plain", "Cache cleared successfully.");
}

void handleResetConfig(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  if (LittleFS.exists("/config.json")) {
    LittleFS.remove("/config.json");
  }
  Serial.println(F("Configuration reset. Rebooting..."));
  request->send(200, "text/plain", "Configuration reset. Rebooting...");
  delay(1000);
  ESP.restart();
}

void handleSwitchMode(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST || !request->hasParam("mode", true)) {
    request->send(400, "text/plain", "Bad Request");
    return;
  }

  String mode = request->getParam("mode", true)->value();
  if (mode == "progress") {
    forcedMode = PROGRESS;
  } else if (mode == "standby") {
    forcedMode = STANDBY;
  } else {
    forcedMode = NONE;
  }
  Serial.print(F("Switched forced mode to: "));
  Serial.println(getForcedModeText(forcedMode));
  request->send(200, "text/plain", "Mode switched successfully.");
}

void handleRestart(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  Serial.println(F("Restarting ESP32..."));
  request->send(200, "text/plain", "Restarting...");
  delay(1000);
  ESP.restart();
}

void handleNotFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not Found");
}