#include "utils.h"
#include "config.h"
#include "mqtt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <esp_system.h>

WiFiUDP udp;

// 记录日志到串口和文件
void appendLog(const String& message) {
    Serial.println(message);
    if (!LittleFS.begin()) return;
    File logFile = LittleFS.open("/log.txt", "a");
    if (logFile) {
        logFile.println("[" + String(millis()) + "] " + message);
        logFile.close();
    }
}

// 恢复出厂设置
void factoryReset() {
    if (LittleFS.begin()) {
        LittleFS.remove("/config.json");
        LittleFS.remove("/log.txt");
    }
    appendLog("执行恢复出厂设置");
}

// 硬重启
void hardReset() {
    appendLog("执行硬重启");
    ESP.restart();
}

// 重启到 Bootloader
void rebootToBootloader() {
    appendLog("重启到 Bootloader");
    delay(100); // 确保日志写入
    esp_restart();
}

// 检查 WiFi 连接状态
void checkWiFiConnection() {
    if (WiFi.status() != WL_CONNECTED) {
        appendLog("WiFi 断开，尝试重新连接");
        WiFi.reconnect();
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            appendLog("WiFi 重新连接成功，IP: " + WiFi.localIP().toString());
        } else {
            appendLog("WiFi 重新连接失败");
        }
    }
}

// 广播设备 IP 地址
void broadcastIP() {
    if (WiFi.status() != WL_CONNECTED) return;
    String ip = WiFi.localIP().toString();
    udp.beginPacket("255.255.255.255", 8888);
    udp.write(reinterpret_cast<const uint8_t*>(ip.c_str()), ip.length());
    udp.endPacket();
    appendLog("广播 IP: " + ip);
}