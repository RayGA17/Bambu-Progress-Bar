#include "mqtt.h"
#include "config.h"
#include "utils.h"
#include "led.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

WiFiClient wifiClient;
PubSubClient client(wifiClient);
JsonDocument printerState;

static const char* MQTT_BROKER = "mqtt.bambulab.com";
static const int MQTT_PORT = 8883;
static unsigned long lastPushall = 0;
static const long PUSHALL_INTERVAL = 5000;

// MQTT 回调函数
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String payloadStr((char*)payload, length);
    appendLog("收到 MQTT 消息，主题: " + String(topic));
    processMqttMessage(payloadStr.c_str(), length);
}

// 初始化 MQTT
void setupMQTT() {
    client.setServer(MQTT_BROKER, MQTT_PORT);
    client.setCallback(mqttCallback);
    if (strlen(uid) > 0 && strlen(accessToken) > 0 && strlen(deviceID) > 0) {
        String clientId = String("BambuLED-") + deviceID;
        String username = uid;
        String password = accessToken;
        if (client.connect(clientId.c_str(), username.c_str(), password.c_str())) {
            appendLog("MQTT 连接成功");
            String topic = String("device/") + deviceID + "/report";
            client.subscribe(topic.c_str());
        } else {
            appendLog("MQTT 连接失败，错误码: " + String(client.state()));
        }
    } else {
        appendLog("MQTT 配置缺失，跳过连接");
    }
}

// 更新 MQTT 状态
void updateMQTT() {
    if (!client.connected()) {
        if (strlen(uid) > 0 && strlen(accessToken) > 0 && strlen(deviceID) > 0) {
            String clientId = String("BambuLED-") + deviceID;
            String username = uid;
            String password = accessToken;
            if (client.connect(clientId.c_str(), username.c_str(), password.c_str())) {
                appendLog("MQTT 重新连接成功");
                String topic = String("device/") + deviceID + "/report";
                client.subscribe(topic.c_str());
            }
        }
    }
    client.loop();

    if (millis() - lastPushall > (customPushallInterval > 0 ? customPushallInterval : PUSHALL_INTERVAL)) {
        sendPushall();
        lastPushall = millis();
    }
}

// 发送 MQTT Pushall 消息
void sendPushall() {
    if (!client.connected()) return;
    JsonDocument doc;
    doc["state"] = getStateText(getState());
    doc["forcedMode"] = getForcedModeText(getForcedMode());
    doc["printPercent"] = printPercent;
    doc["gcodeState"] = gcodeState;
    doc["remainingTime"] = remainingTime;
    doc["layerNum"] = layerNum;
    doc["totalLayerNum"] = totalLayerNum;
    doc["nozzleTemper"] = nozzleTemper;
    doc["bedTemper"] = bedTemper;
    doc["chamberTemper"] = chamberTemper;
    doc["wifiSignal"] = wifiSignal;
    doc["spdLvl"] = spdLvl;
    JsonDocument ledDoc;
    deserializeJson(ledDoc, getLedStatus());
    doc["led"] = ledDoc;
    String output;
    serializeJson(doc, output);
    String topic = String("device/") + deviceID + "/pushall";
    client.publish(topic.c_str(), output.c_str());
    appendLog("发送 MQTT Pushall");
}

// 处理 MQTT 消息
void processMqttMessage(const char *payload, unsigned int length) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) {
        appendLog("MQTT 消息解析失败: " + String(error.code()));
        return;
    }

    if (!doc["print"].isNull()) {
        JsonObject print = doc["print"];
        if (!print["gcode_state"].isNull()) gcodeState = print["gcode_state"].as<String>();
        if (!print["mc_percent"].isNull()) printPercent = print["mc_percent"].as<int>();
        if (!print["mc_remaining_time"].isNull()) remainingTime = print["mc_remaining_time"].as<int>();
        if (!print["layer_num"].isNull()) layerNum = print["layer_num"].as<int>();
        if (!print["total_layer_num"].isNull()) totalLayerNum = print["total_layer_num"].as<int>();
        if (!print["nozzle_temper"].isNull()) nozzleTemper = print["nozzle_temper"].as<float>();
        if (!print["bed_temper"].isNull()) bedTemper = print["bed_temper"].as<float>();
        if (!print["chamber_temper"].isNull()) chamberTemper = print["chamber_temper"].as<float>();
        if (!print["wifi_signal"].isNull()) wifiSignal = print["wifi_signal"].as<String>();
        if (!print["spd_lvl"].isNull()) spdLvl = print["spd_lvl"].as<int>();
        printerState = doc;
        appendLog("更新打印机状态");
    }
}