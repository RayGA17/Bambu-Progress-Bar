#include "ble.h"
#include "config.h"
#include "utils.h"
#include "mqtt.h"
#include "led.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool deviceConnected = false;

// BLE 服务器回调
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
        appendLog("BLE 设备已连接");
    }
    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
        appendLog("BLE 设备已断开");
        pServer->startAdvertising();
    }
};

// BLE 特性写回调
class CommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            handleBLECommand(String(value.c_str()));
        }
    }
};

// 初始化 BLE 服务
void setupBLE() {
    BLEDevice::init("BambuLED");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    BLEService* pService = pServer->createService(BLEUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
    pCharacteristic = pService->createCharacteristic(
        BLEUUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"),
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pCharacteristic->setCallbacks(new CommandCallbacks());
    pService->start();
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLEUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();
    appendLog("BLE 服务已启动");
}

// 更新 BLE 状态
void updateBLE() {
    if (!deviceConnected) {
        if (pServer && !pServer->getAdvertising()->isAdvertising()) {
            pServer->startAdvertising();
        }
    }
}

// 处理 BLE 命令
void handleBLECommand(const String& command) {
    appendLog("收到 BLE 命令: " + command);
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, command);
    if (error) {
        appendLog("BLE 命令解析失败: " + String(error.code()));
        return;
    }

    String action = doc["action"].as<const char*>();
    if (action == "get_status") {
        String response = getBLEStatusResponse();
        pCharacteristic->setValue(response.c_str());
        pCharacteristic->notify();
        appendLog("BLE 发送状态响应");
    } else if (action == "get_config") {
        String response = getBLEConfigResponse();
        pCharacteristic->setValue(response.c_str());
        pCharacteristic->notify();
        appendLog("BLE 发送配置响应");
    } else if (action == "set_config") {
        if (!doc["uid"].isNull()) strlcpy(uid, doc["uid"].as<const char*>(), sizeof(uid));
        if (!doc["accessToken"].isNull()) strlcpy(accessToken, doc["accessToken"].as<const char*>(), sizeof(accessToken));
        if (!doc["deviceID"].isNull()) strlcpy(deviceID, doc["deviceID"].as<const char*>(), sizeof(deviceID));
        if (!doc["customPushallInterval"].isNull()) customPushallInterval = doc["customPushallInterval"].as<int>();
        if (!doc["progressBarColor"].isNull()) progressBarColor = doc["progressBarColor"].as<uint32_t>();
        if (!doc["standbyBreathingColor"].isNull()) standbyBreathingColor = doc["standbyBreathingColor"].as<uint32_t>();
        if (!doc["progressBarBrightnessRatio"].isNull()) progressBarBrightnessRatio = doc["progressBarBrightnessRatio"].as<float>();
        if (!doc["standbyBrightnessRatio"].isNull()) standbyBrightnessRatio = doc["standbyBrightnessRatio"].as<float>();
        if (!doc["standbyMode"].isNull()) strlcpy(standbyMode, doc["standbyMode"].as<const char*>(), sizeof(standbyMode));
        if (!doc["overlayMarquee"].isNull()) overlayMarquee = doc["overlayMarquee"].as<bool>();
        if (!doc["globalBrightness"].isNull()) globalBrightness = doc["globalBrightness"].as<uint8_t>();
        saveConfig();
        appendLog("BLE 配置更新成功");
    } else if (action == "set_force") {
        String mode = doc["mode"].as<const char*>();
        if (mode == "NONE") setForcedMode(NONE);
        else if (mode == "PROGRESS") setForcedMode(PROGRESS);
        else if (mode == "STANDBY") setForcedMode(STANDBY);
        else if (mode == "AP_MODE") setForcedMode(AP_MODE_F);
        else if (mode == "CONNECTING_WIFI") setForcedMode(CONNECTING_WIFI_F);
        else if (mode == "CONNECTED_WIFI") setForcedMode(CONNECTED_WIFI_F);
        else if (mode == "CONNECTING_PRINTER") setForcedMode(CONNECTING_PRINTER_F);
        else if (mode == "CONNECTED_PRINTER") setForcedMode(CONNECTED_PRINTER_F);
        else if (mode == "PRINTING") setForcedMode(PRINTING_F);
        else if (mode == "ERROR") setForcedMode(FAILED_F);
        else {
            appendLog("BLE 无效强制模式: " + mode);
            return;
        }
        appendLog("BLE 设置强制模式: " + mode);
    } else if (action == "test_led") {
        testingLed = true;
        testLedIndex = 0;
        appendLog("BLE 启动 LED 测试");
    } else if (action == "reset") {
        appendLog("BLE 请求软重启");
        delay(100);
        ESP.restart();
    } else if (action == "hard_reset") {
        appendLog("BLE 请求硬重启");
        hardReset();
    } else if (action == "reboot_to_bootloader") {
        appendLog("BLE 请求重启到 Bootloader");
        rebootToBootloader();
    } else if (action == "factory_reset") {
        appendLog("BLE 请求恢复出厂设置");
        factoryReset();
        delay(100);
        ESP.restart();
    }
}

// 获取 BLE 状态响应
String getBLEStatusResponse() {
    StaticJsonDocument<512> doc;
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

    // 解析 LED 状态
    StaticJsonDocument<256> ledDoc;
    DeserializationError ledError = deserializeJson(ledDoc, getLedStatus());
    if (!ledError) {
        doc["led"] = ledDoc;
    } else {
        doc["led"] = nullptr;
        appendLog("LED 状态解析失败: " + String(ledError.code()));
    }

    String output;
    serializeJson(doc, output);
    return output;
}

// 获取 BLE 配置响应
String getBLEConfigResponse() {
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
    String output;
    serializeJson(doc, output);
    return output;
}

// 占位符：待实现日志响应
String getBLELogResponse() {
    return "{}"; // 临时返回空 JSON
}