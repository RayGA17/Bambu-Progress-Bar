#ifndef CONFIG_H
#define CONFIG_H
#include <Arduino.h>

extern char uid[64];
extern char accessToken[64];
extern char deviceID[32];
extern int customPushallInterval;
extern uint32_t progressBarColor;
extern uint32_t standbyBreathingColor;
extern float progressBarBrightnessRatio;
extern float standbyBrightnessRatio;
extern char standbyMode[16];
extern bool overlayMarquee;
extern uint8_t globalBrightness;

// Printer state variables
extern int printPercent;
extern String gcodeState;
extern int remainingTime;
extern int layerNum;
extern int totalLayerNum;
extern float nozzleTemper;
extern float bedTemper;
extern float chamberTemper;
extern String wifiSignal;
extern int spdLvl;

void loadConfig();
void saveConfig();

#endif