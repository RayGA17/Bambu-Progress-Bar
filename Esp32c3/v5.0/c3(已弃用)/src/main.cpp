#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <cstddef>
#include <cstdint>
#include "config.h"
#include "mqtt.h"
#include "led.h"
#include "utils.h"
#include "web.h"
#include "ble.h"

// 初始化
void setup() {
    Serial.begin(115200);
    appendLog(F("--- BambuLED 启动 ---"));

    if (!LittleFS.begin()) {
        appendLog(F("LittleFS 挂载失败，格式化..."));
        LittleFS.format();
        LittleFS.begin();
    }

    loadConfig();
    setupLED();

    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(180);
    if (!wifiManager.autoConnect("BambuLED-AP", "12345678")) {
        appendLog(F("WiFi 配置超时，重启..."));
        ESP.restart();
    }
    appendLog(String(F("WiFi 连接成功，IP: ")) + WiFi.localIP().toString());

    udp.begin(8888);
    setupMQTT();
    setupWebServer();
    setupBLE();
}

// 主循环
void loop() {
    updateMQTT();
    updateLED();
    updateBLE();
}