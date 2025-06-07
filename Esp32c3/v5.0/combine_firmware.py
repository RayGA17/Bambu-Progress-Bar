import os

def combine_firmware():
    bootloader_bin = r"C:\Users\Nalani\Documents\PlatformIO\Projects\c3-bootloader\.pio\build\bootloader\firmware.bin"
    main_bin = r"C:\Users\Nalani\Documents\PlatformIO\Projects\c3-main\.pio\build\esp32-c3-mini-1-h4\firmware.bin"
    combined_bin = r"C:\Users\Nalani\Documents\PlatformIO\Projects\c3-combined\firmware.bin"

    os.makedirs(os.path.dirname(combined_bin), exist_ok=True)

    with open(combined_bin, "wb") as combined_file:
        # 写入 Bootloader（0x0000）
        with open(bootloader_bin, "rb") as bootloader_file:
            combined_file.write(bootloader_file.read())
        # 填充到 0x10000
        combined_file.write(b"\xFF" * (0x10000 - os.path.getsize(bootloader_bin)))
        # 写入主程序（0x10000）
        with open(main_bin, "rb") as main_file:
            combined_file.write(main_file.read())

    print(f"Combined firmware saved to {combined_bin}")
    return combined_bin

if __name__ == "__main__":
    combine_firmware()