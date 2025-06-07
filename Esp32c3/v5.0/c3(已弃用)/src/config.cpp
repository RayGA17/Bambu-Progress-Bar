#include "config.h"
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "utils.h"

// 配置变量
char uid[64] = "";
char accessToken[64] = "";
char deviceID[32] = "";
int customPushallInterval = 5000;
uint32_t progressBarColor = 0xFF0000;
uint32_t standbyBreathingColor = 0x00FF00;
float progressBarBrightnessRatio = 1.0;
float standbyBrightnessRatio = 0.5;
char standbyMode[16] = "breathing";
bool overlayMarquee = false;
uint8_t globalBrightness = 255;

// 打印机状态变量
int printPercent = 0;
String gcodeState = "IDLE";
int remainingTime = 0;
int layerNum = 0;
int totalLayerNum = 0;
float nozzleTemper = 0.0;
float bedTemper = 0.0;
float chamberTemper = 0.0;
String wifiSignal = "";
int spdLvl = 1;

// 加载配置文件
void loadConfig() {
    if (!LittleFS.begin()) {
        appendLog(F("LittleFS 挂载失败，无法加载配置"));
        return;
    }

    File file = LittleFS.open("/config.json", "r");
    if (!file) {
        appendLog(F("未找到配置文件，使用默认值"));
        saveConfig();
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        appendLog(String(F("配置解析失败：")) + String(error.code()));
        return;
    }

    strlcpy(uid, doc["uid"] | "", sizeof(uid));
    strlcpy(accessToken, doc["accessToken"] | "", sizeof(accessToken));
    strlcpy(deviceID, doc["deviceID"] | "", sizeof(deviceID));
    customPushallInterval = doc["customPushallInterval"] | 5000;
    progressBarColor = doc["progressBarColor"] | 0xFF0000;
    standbyBreathingColor = doc["standbyBreathingColor"] | 0x00FF00;
    progressBarBrightnessRatio = doc["progressBarBrightnessRatio"] | 1.0;
    standbyBrightnessRatio = doc["standbyBrightnessRatio"] | 0.5;
    strlcpy(standbyMode, doc["standbyMode"] | "breathing", sizeof(standbyMode));
    overlayMarquee = doc["overlayMarquee"] | false;
    globalBrightness = doc["globalBrightness"] | 255;

    appendLog(F("配置加载成功"));
}

// 保存配置文件
void saveConfig() {
    if (!LittleFS.begin()) {
        appendLog(F("LittleFS 挂载失败，无法保存配置"));
        return;
    }

    StaticJsonDocument<512> doc;
    doc["uid"] = uid;
    doc["accessToken"] = accessToken;
    doc["deviceID"] = deviceID;
    doc["customPushallInterval"] = customPushallInterval;
    doc["progressBarColor"] = progressBarColor;
    doc["standbyBreathingColor"] = standbyBreathingColor;
    doc["progressBarBrightnessRatio"] = progressBarBrightnessRatio;
    doc["standbyBrightnessRatio"] = standbyBrightnessRatio;
    doc["standbyMode"] = standbyMode;
    doc["overlayMarquee"] = overlayMarquee;
    doc["globalBrightness"] = globalBrightness;

    File file = LittleFS.open("/config.json", "w");
    if (!file) {
        appendLog(F("无法创建配置文件"));
        return;
    }

    serializeJson(doc, file);
    file.close();
    appendLog(F("配置保存成功"));
}