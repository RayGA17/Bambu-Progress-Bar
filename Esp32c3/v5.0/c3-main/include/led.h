#ifndef LED_H
#define LED_H
#include <Arduino.h>
#define LED_COUNT 60
#define LED_PIN 8
enum State {
    AP_MODE, CONNECTING_WIFI, CONNECTED_WIFI, CONNECTING_PRINTER,
    CONNECTED_PRINTER, PRINTING, FAILED
};
enum ForcedMode {
    NONE, PROGRESS, STANDBY, AP_MODE_F, CONNECTING_WIFI_F, CONNECTED_WIFI_F,
    CONNECTING_PRINTER_F, CONNECTED_PRINTER_F, PRINTING_F, FAILED_F
};
void setupLED();
void updateLED();
String getLedStatus();
void setState(State state);
State getState();
void setForcedMode(ForcedMode mode);
ForcedMode getForcedMode();
String getStateText(State state);
String getForcedModeText(ForcedMode mode);
extern bool testingLed;
extern int testLedIndex;
#endif