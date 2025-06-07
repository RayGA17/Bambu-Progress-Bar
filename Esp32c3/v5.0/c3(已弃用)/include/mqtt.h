#ifndef MQTT_H
#define MQTT_H
#include <Arduino.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

void setupMQTT();
void updateMQTT();
void sendPushall();
void processMqttMessage(const char *payload, unsigned int length);
extern PubSubClient client;
extern JsonDocument printerState;

#endif