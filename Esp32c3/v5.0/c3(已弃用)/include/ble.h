#ifndef BLE_H
#define BLE_H
#include <Arduino.h>
#include <NimBLEDevice.h>

void setupBLE();
void updateBLE();
void handleBLECommand(const String& cmd);
String getBLEStatusResponse();
String getBLEConfigResponse();
String getBLELogResponse();
void hardReset();
void rebootToBootloader();
void factoryReset();

#endif