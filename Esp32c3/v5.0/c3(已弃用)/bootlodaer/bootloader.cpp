#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <FS.h>
#include <LittleFS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

#define BOOTLOADER_SSID "BambuLED-BL"
#define BOOTLOADER_PASS "12345678"
#define SERVICE_UUID    "4fafc202-1fb5-459e-8fcc-c5c9c331914b"
#define COMMAND_UUID    "beb54850-36e1-4688-b7f5-ea07361b26a8"
#define STATUS_UUID     "beb54851-36e1-4688-b7f5-ea07361b26a8"
#define FIRMWARE_UUID   "beb54852-36e1-4688-b7f5-ea07361b26a8"
#define BOOTLOADER_UUID "beb54853-36e1-4688-b7f5-ea07361b26a8"

AsyncWebServer server(80);
NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pCommandCharacteristic = nullptr;
NimBLECharacteristic *pStatusCharacteristic = nullptr;
NimBLECharacteristic *pFirmwareCharacteristic = nullptr;
NimBLECharacteristic *pBootloaderCharacteristic = nullptr;

static bool deviceConnected = false;
static bool isUpdating = false;
static File tempFile;

// 记录 Bootloader 日志
void appendBootloaderLog(const String& message) {
    Serial.println(message);
    if (!LittleFS.begin()) return;
    File logFile = LittleFS.open("/bootloader.log", "a");
    if (logFile) {
        logFile.println("[" + String(millis()) + "] " + message);
        logFile.close();
    }
}

// 开始固件更新
bool startFirmwareUpdate() {
    if (isUpdating) return false;
    isUpdating = true;
    if (!LittleFS.begin()) {
        appendBootloaderLog(F("LittleFS 挂载失败"));
        return false;
    }
    tempFile = LittleFS.open("/firmware.bin", "w");
    if (!tempFile) {
        appendBootloaderLog(F("无法创建固件临时文件"));
        return false;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        appendBootloaderLog(F("固件更新初始化失败"));
        return false;
    }
    appendBootloaderLog(F("固件更新开始"));
    return true;
}

// 写入固件数据
bool writeFirmware(uint8_t *data, size_t len) {
    if (!isUpdating || !tempFile) return false;
    if (tempFile.write(data, len) != len) {
        appendBootloaderLog(F("固件写入临时文件失败"));
        return false;
    }
    if (Update.write(data, len) != len) {
        appendBootloaderLog(F("固件写入 Flash 失败"));
        return false;
    }
    return true;
}

// 结束固件更新
bool endFirmwareUpdate() {
    if (!isUpdating || !tempFile) return false;
    tempFile.close();
    if (Update.end(true)) {
        LittleFS.remove("/firmware.bin");
        isUpdating = false;
        appendBootloaderLog(F("固件更新完成"));
        return true;
    }
    LittleFS.remove("/firmware.bin");
    isUpdating = false;
    appendBootloaderLog(F("固件更新失败"));
    return false;
}

// 开始 Bootloader 更新
bool startBootloaderUpdate() {
    if (isUpdating) return false;
    isUpdating = true;
    if (!LittleFS.begin()) {
        appendBootloaderLog(F("LittleFS 挂载失败"));
        return false;
    }
    tempFile = LittleFS.open("/bootloader.bin", "w");
    if (!tempFile) {
        appendBootloaderLog(F("无法创建 Bootloader 临时文件"));
        return false;
    }
    if (!Update.begin(0x7000, U_FLASH)) {
        appendBootloaderLog(F("Bootloader 更新初始化失败"));
        return false;
    }
    appendBootloaderLog(F("Bootloader 更新开始"));
    return true;
}

// 写入 Bootloader 数据
bool writeBootloader(uint8_t *data, size_t len) {
    if (!isUpdating || !tempFile) return false;
    if (tempFile.write(data, len) != len) {
        appendBootloaderLog(F("Bootloader 写入临时文件失败"));
        return false;
    }
    if (Update.write(data, len) != len) {
        appendBootloaderLog(F("Bootloader 写入 Flash 失败"));
        return false;
    }
    return true;
}

// 结束 Bootloader 更新
bool endBootloaderUpdate() {
    if (!isUpdating || !tempFile) return false;
    tempFile.close();
    if (Update.end(true)) {
        LittleFS.remove("/bootloader.bin");
        isUpdating = false;
        appendBootloaderLog(F("Bootloader 更新完成"));
        return true;
    }
    LittleFS.remove("/bootloader.bin");
    isUpdating = false;
    appendBootloaderLog(F("Bootloader 更新失败"));
    return false;
}

// 恢复出厂设置
void factoryReset() {
    LittleFS.remove("/config.json");
    LittleFS.remove("/log.txt");
    LittleFS.remove("/bootloader.log");
    appendBootloaderLog(F("执行恢复出厂设置"));
}

// 设置 WiFi AP
void setupWiFi() {
    WiFi.softAP(BOOTLOADER_SSID, BOOTLOADER_PASS);
    appendBootloaderLog(String(F("Bootloader WiFi AP 启动，SSID: ")) + BOOTLOADER_SSID);
}

// 设置 Web 服务器
void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", "<h1>BambuLED Bootloader</h1><p>请使用 API 上传固件或管理设备</p>");
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"mode\":\"bootloader\"}");
    });

    server.on("/api/factoryReset", HTTP_POST, [](AsyncWebServerRequest *request) {
        appendBootloaderLog(F("恢复出厂设置"));
        factoryReset();
        request->send(200, "application/json", "{\"status\":\"success\"}");
        delay(100);
        ESP.restart();
    });

    server.on("/api/uploadFirmware", HTTP_POST, [](AsyncWebServerRequest *request) {}, 
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                if (!startFirmwareUpdate()) {
                    request->send(500, "application/json", "{\"error\":\"无法启动更新\"}");
                    return;
                }
            }
            if (writeFirmware(data, len)) {
                if (final) {
                    if (endFirmwareUpdate()) {
                        appendBootloaderLog(F("固件上传完成"));
                        request->send(200, "application/json", "{\"status\":\"success\"}");
                    } else {
                        request->send(500, "application/json", "{\"error\":\"固件写入失败\"}");
                    }
                }
            } else {
                request->send(500, "application/json", "{\"error\":\"写入错误\"}");
            }
    });

    server.on("/api/uploadBootloader", HTTP_POST, [](AsyncWebServerRequest *request) {}, 
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                if (!startBootloaderUpdate()) {
                    request->send(500, "application/json", "{\"error\":\"无法启动更新\"}");
                    return;
                }
            }
            if (writeBootloader(data, len)) {
                if (final) {
                    if (endBootloaderUpdate()) {
                        appendBootloaderLog(F("Bootloader 上传完成"));
                        request->send(200, "application/json", "{\"status\":\"success\"}");
                    } else {
                        request->send(500, "application/json", "{\"error\":\"Bootloader 写入失败\"}");
                    }
                }
            } else {
                request->send(500, "application/json", "{\"error\":\"写入错误\"}");
            }
    });

    server.begin();
    appendBootloaderLog(F("Bootloader Web 服务器启动"));
}

// BLE 命令回调
class CommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, value.c_str());
        if (!error) {
            String action = doc["action"].as<const char*>();
            if (action == "status") {
                pStatusCharacteristic->setValue("{\"mode\":\"bootloader\"}");
                pStatusCharacteristic->notify();
            } else if (action == "factoryReset") {
                appendBootloaderLog(F("蓝牙恢复出厂设置"));
                factoryReset();
                pStatusCharacteristic->setValue("{\"status\":\"success\"}");
                pStatusCharacteristic->notify();
                delay(100);
                ESP.restart();
            }
        } else {
            appendBootloaderLog("BLE 命令解析失败: " + String(error.code()));
        }
    }
};

// BLE 固件更新回调
class FirmwareCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (!isUpdating) {
            isUpdating = startFirmwareUpdate();
            appendBootloaderLog(isUpdating ? F("蓝牙固件更新开始") : F("蓝牙固件更新失败"));
        }
        if (isUpdating) {
            writeFirmware((uint8_t*)value.c_str(), value.length());
            if (value.length() >= 3 && value.substr(value.length() - 3) == "END") {
                if (endFirmwareUpdate()) {
                    appendBootloaderLog(F("蓝牙固件更新完成"));
                    pStatusCharacteristic->setValue("{\"status\":\"success\"}");
                    pStatusCharacteristic->notify();
                } else {
                    appendBootloaderLog(F("蓝牙固件更新失败"));
                    pStatusCharacteristic->setValue("{\"error\":\"固件更新失败\"}");
                    pStatusCharacteristic->notify();
                }
                isUpdating = false;
            }
        }
    }
};

// BLE Bootloader 更新回调
class BootloaderCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (!isUpdating) {
            isUpdating = startBootloaderUpdate();
            appendBootloaderLog(isUpdating ? F("蓝牙 Bootloader 更新开始") : F("蓝牙 Bootloader 更新失败"));
        }
        if (isUpdating) {
            writeBootloader((uint8_t*)value.c_str(), value.length());
            if (value.length() >= 3 && value.substr(value.length() - 3) == "END") {
                if (endBootloaderUpdate()) {
                    appendBootloaderLog(F("蓝牙 Bootloader 更新完成"));
                    pStatusCharacteristic->setValue("{\"status\":\"success\"}");
                    pStatusCharacteristic->notify();
                } else {
                    appendBootloaderLog(F("蓝牙 Bootloader 更新失败"));
                    pStatusCharacteristic->setValue("{\"error\":\"Bootloader 更新失败\"}");
                    pStatusCharacteristic->notify();
                }
                isUpdating = false;
            }
        }
    }
};

// BLE 服务器回调
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer) override {
        deviceConnected = true;
        appendBootloaderLog(F("蓝牙设备已连接"));
    }
    void onDisconnect(NimBLEServer *pServer) override {
        deviceConnected = false;
        appendBootloaderLog(F("蓝牙设备已断开"));
        NimBLEDevice::startAdvertising();
    }
};

// 设置 BLE 服务
void setupBLE() {
    NimBLEDevice::init("BambuLED-BL");
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    NimBLEService *pService = pServer->createService(SERVICE_UUID);
    pCommandCharacteristic = pService->createCharacteristic(COMMAND_UUID, NIMBLE_PROPERTY::WRITE);
    pStatusCharacteristic = pService->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    pFirmwareCharacteristic = pService->createCharacteristic(FIRMWARE_UUID, NIMBLE_PROPERTY::WRITE);
    pBootloaderCharacteristic = pService->createCharacteristic(BOOTLOADER_UUID, NIMBLE_PROPERTY::WRITE);

    pCommandCharacteristic->setCallbacks(new CommandCallbacks());
    pFirmwareCharacteristic->setCallbacks(new FirmwareCallbacks());
    pBootloaderCharacteristic->setCallbacks(new BootloaderCallbacks());
    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();
    appendBootloaderLog(F("Bootloader 蓝牙服务启动"));
}

// 初始化
void setup() {
    Serial.begin(115200);
    appendBootloaderLog(F("--- BambuLED Bootloader 启动 ---"));

    if (!LittleFS.begin()) {
        appendBootloaderLog(F("LittleFS 挂载失败，格式化..."));
        LittleFS.format();
        LittleFS.begin();
    }

    setupWiFi();
    setupWebServer();
    setupBLE();
}

// 主循环
void loop() {
    delay(10);
}