#ifndef OTA_H
#define OTA_H
#include <cstddef>
#include <cstdint>

bool startFirmwareUpdate();
bool writeFirmware(uint8_t *data, size_t len);
bool endFirmwareUpdate();
bool startBootloaderUpdate();
bool writeBootloader(uint8_t *data, size_t len);
bool endBootloaderUpdate();

#endif