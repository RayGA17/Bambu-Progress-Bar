#include "ota.h"
#include "utils.h"
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Update.h>

static bool isUpdating = false;
static File tempFile;

// 开始固件更新
bool startFirmwareUpdate() {
    if (isUpdating) return false;
    isUpdating = true;
    if (!LittleFS.begin()) {
        appendLog(F("LittleFS 挂载失败，无法启动固件更新"));
        return false;
    }
    tempFile = LittleFS.open("/firmware.bin", "w");
    if (!tempFile) {
        appendLog(F("无法创建固件临时文件"));
        return false;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        appendLog(F("固件更新初始化失败"));
        return false;
    }
    appendLog(F("固件更新开始"));
    return true;
}

// 写入固件数据
bool writeFirmware(uint8_t *data, size_t len) {
    if (!isUpdating || !tempFile) return false;
    if (tempFile.write(data, len) != len) {
        appendLog(F("固件写入临时文件失败"));
        return false;
    }
    if (Update.write(data, len) != len) {
        appendLog(F("固件写入 Flash 失败"));
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
        appendLog(F("固件更新完成"));
        return true;
    }
    LittleFS.remove("/firmware.bin");
    isUpdating = false;
    appendLog(F("固件更新失败"));
    return false;
}

// 开始 Bootloader 更新
bool startBootloaderUpdate() {
    if (isUpdating) return false;
    isUpdating = true;
    if (!LittleFS.begin()) {
        appendLog(F("LittleFS 挂载失败，无法启动 Bootloader 更新"));
        return false;
    }
    tempFile = LittleFS.open("/bootloader.bin", "w");
    if (!tempFile) {
        appendLog(F("无法创建 Bootloader 临时文件"));
        return false;
    }
    if (!Update.begin(0x7000, U_FLASH)) {
        appendLog(F("Bootloader 更新初始化失败"));
        return false;
    }
    appendLog(F("Bootloader 更新开始"));
    return true;
}

// 写入 Bootloader 数据
bool writeBootloader(uint8_t *data, size_t len) {
    if (!isUpdating || !tempFile) return false;
    if (tempFile.write(data, len) != len) {
        appendLog(F("Bootloader 写入临时文件失败"));
        return false;
    }
    if (Update.write(data, len) != len) {
        appendLog(F("Bootloader 写入 Flash 失败"));
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
        appendLog(F("Bootloader 更新完成"));
        return true;
    }
    LittleFS.remove("/bootloader.bin");
    isUpdating = false;
    appendLog(F("Bootloader 更新失败"));
    return false;
}