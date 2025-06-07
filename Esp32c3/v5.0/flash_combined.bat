@echo off
echo Compiling c3-bootloader...
cd C:\Users\Nalani\Documents\PlatformIO\Projects\c3-bootloader
pio run -t clean
pio run

echo Compiling c3-main...
cd C:\Users\Nalani\Documents\PlatformIO\Projects\c3-main
pio run -t clean
pio run

echo Combining firmware...
cd C:\Users\Nalani\Documents\PlatformIO\Projects\c3-combined
python combine_firmware.py

echo Flashing combined firmware...
esptool.py --chip esp32c3 --port COMX --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x0000 C:\Users\Nalani\Documents\PlatformIO\Projects\c3-combined\firmware.bin

echo Uploading LittleFS...
cd C:\Users\Nalani\Documents\PlatformIO\Projects\c3-main
pio run -t uploadfs

echo Done!
pause