#include "web.h"
#include "config.h"
#include "utils.h"
#include "mqtt.h"
#include "led.h"
#include "ota.h"
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

AsyncWebServer server(80);

void setupWebServer() {
    if (!LittleFS.begin()) {
        appendLog(F("LittleFS 挂载失败"));
        return;
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/index.html", "text/html");
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<384> doc; // 减少 JSON 缓冲区
        doc["status_text"] = getStateText(getState());
        doc["forced_mode"] = getForcedModeText(getForcedMode());
        doc["print_percent"] = printPercent;
        doc["gcode_state"] = gcodeState;
        doc["remaining_time"] = remainingTime;
        doc["layer_num"] = layerNum;
        doc["total_layer_num"] = totalLayerNum;
        doc["nozzle_temper"] = nozzleTemper;
        doc["bed_temper"] = bedTemper;
        doc["chamber_temper"] = chamberTemper;
        doc["wifi_signal"] = wifiSignal;
        doc["spd_lvl"] = spdLvl;
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server.on("/getConfig", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<384> doc; // 减少 JSON 缓冲区
        doc["uid"] = uid;
        doc["accessToken"] = accessToken;
        doc["deviceID"] = deviceID;
        doc["globalBrightness"] = globalBrightness;
        doc["standbyMode"] = standbyMode;
        doc["progressBarColor"] = progressBarColor;
        doc["standbyBreathingColor"] = standbyBreathingColor;
        doc["progressBarBrightnessRatio"] = progressBarBrightnessRatio;
        doc["standbyBrightnessRatio"] = standbyBrightnessRatio;
        doc["customPushallInterval"] = customPushallInterval;
        doc["overlayMarquee"] = overlayMarquee;
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("uid", true) || !request->hasParam("accessToken", true) || !request->hasParam("deviceID", true)) {
            request->send(400, "text/plain", "缺少必要参数");
            return;
        }

        strlcpy(uid, request->getParam("uid", true)->value().c_str(), sizeof(uid));
        strlcpy(accessToken, request->getParam("accessToken", true)->value().c_str(), sizeof(accessToken));
        strlcpy(deviceID, request->getParam("deviceID", true)->value().c_str(), sizeof(deviceID));
        globalBrightness = request->hasParam("globalBrightness", true) ? request->getParam("globalBrightness", true)->value().toInt() : 255;
        strlcpy(standbyMode, request->hasParam("standbyMode", true) ? request->getParam("standbyMode", true)->value().c_str() : "breathing", sizeof(standbyMode));
        String progressColor = request->hasParam("progressBarColor", true) ? request->getParam("progressBarColor", true)->value() : "FFFFFF";
        progressBarColor = strtoul(progressColor.c_str(), nullptr, 16);
        String standbyColor = request->hasParam("standbyBreathingColor", true) ? request->getParam("standbyBreathingColor", true)->value() : "FFFFFF";
        standbyBreathingColor = strtoul(standbyColor.c_str(), nullptr, 16);
        progressBarBrightnessRatio = request->hasParam("progressBarBrightnessRatio", true) ? request->getParam("progressBarBrightnessRatio", true)->value().toFloat() : 1.0;
        standbyBrightnessRatio = request->hasParam("standbyBrightnessRatio", true) ? request->getParam("standbyBrightnessRatio", true)->value().toFloat() : 1.0;
        customPushallInterval = request->hasParam("customPushallInterval", true) ? request->getParam("customPushallInterval", true)->value().toInt() : 10000;
        overlayMarquee = request->hasParam("overlayMarquee", true);

        saveConfig();
        appendLog(F("Web 配置更新成功"));
        request->send(200, "text/plain", "配置已保存");
        delay(100);
        ESP.restart();
    });

    server.on("/switchMode", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("mode", true)) {
            request->send(400, "text/plain", "缺少模式参数");
            return;
        }
        String mode = request->getParam("mode", true)->value();
        if (mode == "progress") {
            setForcedMode(PROGRESS);
        } else if (mode == "standby") {
            setForcedMode(STANDBY);
        } else if (mode == "none") {
            setForcedMode(NONE);
        } else {
            request->send(400, "text/plain", "无效模式");
            return;
        }
        appendLog("Web 设置强制模式: " + mode);
        request->send(200, "text/plain", "模式已切换");
    });

    server.on("/testLed", HTTP_POST, [](AsyncWebServerRequest *request) {
        testingLed = true;
        testLedIndex = 0;
        appendLog(F("Web 启动 LED 测试"));
        request->send(200, "text/plain", "LED 测试完成");
    });

    server.on("/clearCache", HTTP_POST, [](AsyncWebServerRequest *request) {
        WiFi.disconnect(true);
        appendLog(F("Web 清除缓存"));
        request->send(200, "text/plain", "缓存已清除");
    });

    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        LittleFS.remove("/config.json");
        appendLog(F("Web 重置配置"));
        request->send(200, "text/plain", "配置已重置");
        delay(100);
        ESP.restart();
    });

    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        appendLog(F("Web 请求重启"));
        request->send(200, "text/plain", "设备正在重启");
        delay(500);
        ESP.restart();
    });

    server.on("/hardReset", HTTP_POST, [](AsyncWebServerRequest *request) {
        appendLog(F("Web 请求硬重启"));
        request->send(200, "text/plain", "success");
        delay(100);
        hardReset();
    });

    server.on("/rebootToBootloader", HTTP_POST, [](AsyncWebServerRequest *request) {
        appendLog(F("Web 请求重启到 Bootloader"));
        request->send(200, "text/plain", "success");
        delay(100);
        rebootToBootloader();
    });

    server.on("/factoryReset", HTTP_POST, [](AsyncWebServerRequest *request) {
        appendLog(F("Web 请求恢复出厂设置"));
        factoryReset();
        request->send(200, "text/plain", "success");
        delay(2000);
        ESP.restart();
    });

    server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        File file = LittleFS.open("/log.txt", "r");
        if (!file) {
            request->send(404, "text/plain", "日志文件不存在");
            return;
        }
        String logContent;
        logContent.reserve(file.size());
        while (file.available()) {
            logContent += (char)file.read();
        }
        file.close();
        request->send(200, "text/plain", logContent);
    });

    server.on("/uploadFirmware", HTTP_POST, [](AsyncWebServerRequest *request) {}, 
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index && !startFirmwareUpdate()) {
                request->send(500, "application/json", "{\"error\": \"无法启动更新\"}");
                return;
            }
            if (writeFirmware(data, len)) {
                if (final && endFirmwareUpdate()) {
                    appendLog(F("固件上传完成"));
                    request->send(200, "application/json", "{\"status\": \"success\"}");
                } else if (final) {
                    request->send(500, "application/json", "{\"error\": \"固件写入失败\"}");
                }
            } else {
                request->send(500, "application/json", "{\"error\": \"写入错误\"}");
            }
        });

    server.on("/uploadBootloader", HTTP_POST, [](AsyncWebServerRequest *request) {}, 
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index && !startBootloaderUpdate()) {
                request->send(500, "application/json", "{\"error\": \"无法启动更新\"}");
                return;
            }
            if (writeBootloader(data, len)) {
                if (final && endBootloaderUpdate()) {
                    appendLog(F("Bootloader 上传成功"));
                    request->send(200, "application/json", "{\"status\": \"success\"}");
                } else if (final) {
                    request->send(500, "application/json", "{\"error\": \"Bootloader 写入失败\"}");
                }
            } else {
                request->send(500, "application/json", "{\"error\": \"写入错误\"}");
            }
        });

    server.begin();
    appendLog(F("Web 服务器启动"));
}