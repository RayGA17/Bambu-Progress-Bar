#include "led.h"
#include "config.h"
#include "utils.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
bool testingLed = false;
int testLedIndex = 0;
static State currentState = AP_MODE;
static ForcedMode forcedMode = NONE;

// 设置当前状态
void setState(State state) { currentState = state; }

// 获取当前状态
State getState() { return currentState; }

// 设置强制模式
void setForcedMode(ForcedMode mode) { forcedMode = mode; }

// 获取强制模式
ForcedMode getForcedMode() { return forcedMode; }

// 获取状态文本
String getStateText(State state) {
    switch (state) {
        case AP_MODE: return "AP_MODE";
        case CONNECTING_WIFI: return "CONNECTING_WIFI";
        case CONNECTED_WIFI: return "CONNECTED_WIFI";
        case CONNECTING_PRINTER: return "CONNECTING_PRINTER";
        case CONNECTED_PRINTER: return "CONNECTED_PRINTER";
        case PRINTING: return "PRINTING";
        case FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

// 获取强制模式文本
String getForcedModeText(ForcedMode mode) {
    switch (mode) {
        case NONE: return "NONE";
        case PROGRESS: return "PROGRESS";
        case STANDBY: return "STANDBY";
        case AP_MODE_F: return "AP_MODE";
        case CONNECTING_WIFI_F: return "CONNECTING_WIFI";
        case CONNECTED_WIFI_F: return "CONNECTED_WIFI";
        case CONNECTING_PRINTER_F: return "CONNECTING_PRINTER";
        case CONNECTED_PRINTER_F: return "CONNECTED_PRINTER";
        case PRINTING_F: return "PRINTING";
        case FAILED_F: return "ERROR";
        default: return "UNKNOWN";
    }
}

// 初始化 LED 条
void setupLED() {
    strip.begin();
    strip.setBrightness(globalBrightness);
    strip.show();
    appendLog("LED 初始化完成");
}

// 更新 LED 显示
void updateLED() {
    strip.clear();
    if (testingLed) {
        strip.setPixelColor(testLedIndex, 0xFFFFFF);
        strip.show();
        testLedIndex = (testLedIndex + 1) % LED_COUNT;
        if (testLedIndex == 0) testingLed = false;
        return;
    }

    if (forcedMode != NONE) {
        switch (forcedMode) {
            case PROGRESS:
                for (int i = 0; i < LED_COUNT * printPercent / 100; i++) {
                    strip.setPixelColor(i, progressBarColor);
                }
                strip.setBrightness(globalBrightness * progressBarBrightnessRatio);
                break;
            case STANDBY:
                if (strcmp(standbyMode, "breathing") == 0) {
                    uint8_t brightness = (sin(millis() / 1000.0) * 127.5 + 127.5) * standbyBrightnessRatio;
                    strip.fill(standbyBreathingColor, 0, LED_COUNT);
                    strip.setBrightness(brightness);
                } else {
                    strip.fill(standbyBreathingColor, 0, LED_COUNT);
                    strip.setBrightness(globalBrightness * standbyBrightnessRatio);
                }
                break;
            case AP_MODE_F:
                strip.fill(0x0000FF, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTING_WIFI_F:
                strip.fill(0xFFFF00, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTED_WIFI_F:
                strip.fill(0x00FF00, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTING_PRINTER_F:
                strip.fill(0xFF00FF, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTED_PRINTER_F:
                strip.fill(0x00FFFF, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case PRINTING_F:
                strip.fill(0xFF0000, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case FAILED_F:
                strip.fill(0xFF0000, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            default:
                break;
        }
    } else {
        switch (currentState) {
            case AP_MODE:
                strip.fill(0x0000FF, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTING_WIFI:
                strip.fill(0xFFFF00, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTED_WIFI:
                strip.fill(0x00FF00, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTING_PRINTER:
                strip.fill(0xFF00FF, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case CONNECTED_PRINTER:
                strip.fill(0x00FFFF, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
            case PRINTING:
                for (int i = 0; i < LED_COUNT * printPercent / 100; i++) {
                    strip.setPixelColor(i, progressBarColor);
                }
                strip.setBrightness(globalBrightness * progressBarBrightnessRatio);
                break;
            case FAILED:
                strip.fill(0xFF0000, 0, LED_COUNT);
                strip.setBrightness(globalBrightness);
                break;
        }
    }

    if (overlayMarquee) {
        static int marqueePos = 0;
        strip.setPixelColor(marqueePos, 0xFFFFFF);
        marqueePos = (marqueePos + 1) % LED_COUNT;
    }

    strip.show();
}

// 获取 LED 状态
String getLedStatus() {
    StaticJsonDocument<256> doc;
    doc["testingLed"] = testingLed;
    doc["testLedIndex"] = testLedIndex;
    doc["currentState"] = getStateText(currentState);
    doc["forcedMode"] = getForcedModeText(forcedMode);
    doc["brightness"] = strip.getBrightness();
    String output;
    serializeJson(doc, output);
    return output;
}