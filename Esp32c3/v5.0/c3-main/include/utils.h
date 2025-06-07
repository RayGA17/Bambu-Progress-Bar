#ifndef UTILS_H
#define UTILS_H
#include <Arduino.h>
#include <WiFiUDP.h>

void appendLog(const String& message);
void factoryReset();
void hardReset();
void rebootToBootloader();
void checkWiFiConnection();
void broadcastIP();
extern WiFiUDP udp;

#endif