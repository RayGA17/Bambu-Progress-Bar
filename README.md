# **Bambu Lab Printer Status LED Display / Bambu Lab 打印机 LED 状态显示**

## **English**

This project provides a firmware for ESP8266 and ESP32-C3 microcontrollers to display the status of a Bambu Lab 3D printer using a NeoPixel LED strip. It connects to your WiFi network, communicates with the printer via MQTT, and offers a web interface for configuration.

### **Features**

* **Printer Status Indication:** Displays printer status (e.g., printing progress, standby, error) on an LED strip.  
* **WiFi Configuration:** Uses WiFiManager (ESP8266) or standard WiFi setup (ESP32-C3) for easy network configuration via a captive portal.  
* **MQTT Communication:** Connects to the Bambu Lab MQTT broker to receive real-time printer status.  
* **Web Interface:** Provides a web page for:  
  * Viewing current printer and device status.  
  * Configuring WiFi (initial setup).  
  * Configuring MQTT credentials (UID, Access Token, Device Serial Number).  
  * Customizing LED behavior (brightness, standby modes, colors, overlay effects).  
  * Testing the LED strip.  
  * Resetting configuration or restarting the device.  
* **Persistent Configuration:** Saves settings to LittleFS on the microcontroller.  
* **Customizable LED Effects:**  
  * Progress bar for printing.  
  * Standby modes: Rainbow Marquee, Breathing Light.  
  * Configurable colors and brightness ratios for different states.  
  * Optional marquee overlay during printing.  
* **Watchdog Timer:** Implemented for improved stability (especially in later ESP8266 versions and ESP32-C3).  
* **Memory Optimization:** Later versions utilize PROGMEM for static content to save RAM.  
* **MQTT Buffering (ESP8266 v3.4+ / ESP32-C3):** Buffers MQTT messages to files to handle disconnections or resource constraints more gracefully.

### **Hardware Requirements**

* **Microcontroller:**  
  * ESP8266 (e.g., Wemos D1 Mini, NodeMCU)  
  * ESP32-C3 (e.g., ESP32-C3-DevKitM-1, or a custom board with ESP32-C3 like the Mini-1-H4 mentioned in ESP32-C3 code)  
* **LED Strip:** Adafruit NeoPixel compatible (WS2812B, WS2811, etc.). Default configuration is for 20 LEDs.  
* **Power Supply:** Appropriate power supply for the MCU and LED strip.  
* **Wiring:** Standard connections for MCU, LED strip, and power.

### **Software Requirements**

* **Development Environment:** PlatformIO IDE (recommended) or Arduino IDE.  
* **Libraries (managed by PlatformIO or manually installed in Arduino IDE):**  
  * **Common:**  
    * Adafruit NeoPixel  
    * ArduinoJson (v6.x recommended)  
    * PubSubClient (by knolleary)  
    * LittleFS  
    * WiFiClientSecure  
  * **ESP8266 Specific:**  
    * ESP8266WiFi  
    * WiFiManager (by tzapu)  
    * ESP8266WebServer  
    * Ticker (for watchdog in some versions)  
  * **ESP32-C3 Specific (see platformio.ini for ESP32-C3):**  
    * WiFi (ESP32 core)  
    * WiFiManager (by tzapu, though ESP32 can also use other WiFi provisioning methods)  
    * ESPAsyncWebServer (by me-no-dev)  
    * esp\_task\_wdt (ESP32 core for watchdog)

### **Firmware Versions**

#### **ESP8266 Series**

* **v1.8 & v2.0:**  
  * Base functionality: WiFi connection via WiFiManager, MQTT connection to Bambu Lab, basic LED status display (progress bar, simple standby), and a web interface for configuration.  
  * Configuration includes UID, Access Token, Device ID, global brightness, standby mode (marquee/breathing), progress bar color, standby breathing color, brightness ratios, and marquee overlay.  
  * LED test functionality.  
  * Single device web access control (one client can configure at a time).  
  * Basic watchdog timer.  
  * *Note: v1.8 and v2.0 appear to be functionally identical based on the provided code.*  
* **v2.2:**  
  * **Localization:** Introduced Chinese language for comments and web UI text.  
  * **Enhanced Watchdog:** Implemented a more specific watchdog using Ticker to monitor LED rendering, mode judgment, and low heap memory.  
  * **Configurable Pushall Interval:** Added customPushallInterval for MQTT pushall requests, configurable via the web UI and saved to config.json.  
  * **NTP Sync Timeout:** Added a 5-second timeout for NTP time synchronization.  
  * **Improved LED Effects:**  
    * More precise partial pixel rendering for the progress bar.  
    * Marquee overlay now blends colors with the progress bar.  
  * **Web UI Feedback:** Switched to using JavaScript alert() for configuration save/test feedback and client-side redirection.  
  * **Forced Mode:** Introduced a "Forced Mode" (Progress/Standby) controllable via a web UI endpoint, allowing manual override of the LED display state for testing or preference.  
  * **Detailed Logging:** Enhanced the /log endpoint to provide more comprehensive status information.  
* **v3.0:**  
  * **Web Serving Optimization:**  
    * Major change: Introduced serving a static /index.html file from LittleFS instead of dynamically generating HTML strings in memory. This significantly reduces RAM usage during web requests.  
    * Added isWebServing and pauseLedUpdate flags to manage system resources and prevent LED/MQTT interference during web page delivery.  
    * Implemented sendFileInChunks for serving the static HTML.  
  * **NTP Server Change:** Switched NTP server to ntp.aliyun.com.  
  * **Improved Web Form:** More robust client-side JavaScript validation for form inputs (e.g., color hex format).  
  * **Deferred Pushall:** MQTT pushall requests are deferred if a web page is being served (pendingPushall flag).  
* **v3.2:**  
  * **MQTT Stability:**  
    * Introduced an MQTT response timeout (mqttResponseTimeout). If no MQTT messages are received within this period, the printer is flagged as printerOffline.  
    * The system attempts to reconnect to MQTT if the printer is marked offline.  
  * **Printer Offline Indication:** The printerOffline status is reflected in the web UI and logs.  
  * **MQTT Data Validation:** Added checks in mqttCallback for null or missing "print" data to better detect an offline printer or invalid messages.  
* **v3.4:**  
  * **PROGMEM Optimization:** Extensively used PROGMEM for constant strings (MQTT server/topics, HTML content) to significantly reduce RAM footprint.  
  * **MQTT Message Buffering to File:**  
    * Implemented robust MQTT message buffering to LittleFS for both received (RX) and to-be-sent (TX) messages.  
    * writeMqttRxBuffer and processMqttRxBuffer: Incoming messages are written to MQTT\_RX\_BUFFER\_FILE and processed from there, allowing the system to handle large messages or process them when resources are available.  
    * writeMqttTxBuffer and processMqttTxBuffer: Outgoing messages (like pushall) are written to MQTT\_TX\_BUFFER\_FILE and sent when the MQTT client is ready.  
    * This helps in low-memory situations and improves reliability.  
  * **MQTT Reconnect Backoff:** Implemented exponential backoff for MQTT reconnection attempts (reconnectDelay increases up to maxReconnectDelay).  
  * **Enhanced Web Serving:** sendFileInChunks further optimized for very low memory by sending PROGMEM HTML directly if heap is critically low. Introduced pauseMqttUpdate during web serving.  
  * **Watchdog Refinement:** Watchdog timeout increased; low heap check is skipped during web serving.  
  * **Visual Change:** Marquee flow direction reversed (right to left).  
  * **Web UI Enhancements:** Added a "Restart Device" button. More server-side validation for configuration parameters.

#### **ESP32-C3 Series**

The ESP32-C3 versions build upon the stability and features of the later ESP8266 versions, adapting the codebase for the ESP32 platform.

* **Platform Changes from ESP8266:**  
  * Uses WiFi.h (ESP32 core).  
  * Uses ESPAsyncWebServer for non-blocking web server operations, which is more suitable for the ESP32's capabilities.  
  * Utilizes the ESP32's built-in task watchdog (esp\_task\_wdt).  
  * Uses DynamicJsonDocument with a larger capacity due to more available RAM on ESP32.  
  * Client ID for MQTT is generated using ESP.getEfuseMac().  
* **v4.0:**  
  * **Core Logic Ported:** Most features from ESP8266 v3.4 are ported, including PROGMEM usage for HTML, file-based MQTT buffering, configurable parameters, and LED effects.  
  * **LED Pin:** Configured for GPIO8 on ESP32-C3 Mini-1-H4.  
  * **Web UI:** Adapted to use ESPAsyncWebServer syntax and parameter handling.  
  * **NTP Server:** Uses pool.ntp.org and time.windows.com.  
  * **MQTT Timeout:** Includes an MQTT message timeout; if no messages are received, currentState is set to ERROR.  
  * **Heap Management:** Monitors heap and restarts if it falls below 20,000 bytes.  
  * **Rainbow Color Algorithm:** Uses a slightly different algorithm for getRainbowColor compared to the final ESP8266 version, based on direct hue mapping.  
  * **Marquee Direction:** Flows from left to right.  
* **v4.2:**  
  * **Marquee Direction:** Changed marquee flow direction to right-to-left (same as ESP8266 v3.4).  
  * **Print Completion Behavior:** When a print finishes (gcodeState \== "FINISH" or printPercent \>= 99), any forcedMode is automatically reset to NONE. This improves usability by returning to automatic status display after a forced state.  
  * **Web UI Color Input:** Minor refinements in JavaScript for synchronizing \<input type='color'\> with the text hex input, ensuring the \# prefix is handled consistently.  
  * **Minor Fixes:** Potential minor stability improvements and logging adjustments.

### **Setup and Configuration**

1. **Flashing the Firmware:**  
   * Use PlatformIO to build and upload the desired firmware version to your ESP8266 or ESP32-C3 board.  
   * Ensure you select the correct environment in platformio.ini for your board (especially for ESP32-C3).  
2. **Initial WiFi Configuration:**  
   * On the first boot, or if WiFi credentials are lost, the device will start in Access Point (AP) mode.  
   * The AP SSID will typically be "BambuAP" (ESP8266) or "BambuLED-Setup" (ESP32-C3).  
   * Connect to this AP with your phone or computer. A captive portal should appear (or navigate to 192.168.4.1).  
   * Select your WiFi network (SSID), enter the password.  
   * **For versions that include MQTT setup in WiFiManager (e.g., ESP32-C3 v4.0+ if config is empty):** Enter your Bambu Lab UID, Access Token, and Device Serial Number.  
   * Save the configuration. The device will attempt to connect to your WiFi.  
3. **Web Interface Configuration:**  
   * Once connected to your WiFi, find the device's IP address (e.g., from your router's client list or serial monitor).  
   * Open this IP address in a web browser.  
   * You can configure:  
     * MQTT Credentials (if not set during WiFiManager setup).  
     * LED brightness and color settings for various states.  
     * Standby mode and marquee effects.  
     * MQTT pushall interval.  
   * Save the configuration. Changes are typically applied immediately or after a pushall request.

### **Web Interface**

The web interface (/) allows you to:

* View the current device and printer status.  
* Modify and save all configurable parameters:  
  * UID, Access Token, Device Serial Number.  
  * Global LED Brightness.  
  * Standby Mode (Rainbow Marquee, Breathing Light).  
  * Progress Bar Color.  
  * Standby Breathing Light Color.  
  * Brightness Ratios for Progress Bar and Standby modes.  
  * MQTT pushall request interval.  
  * Overlay Marquee on Progress Bar (checkbox).  
* **Actions:**  
  * Test LED Strip.  
  * Clear Cache (clears MQTT buffer files in later versions).  
  * Reset Configuration (deletes config.json and restarts).  
  * Restart Device (ESP8266 v3.4+, ESP32-C3).  
  * Switch Mode (Force Progress Bar, Force Standby, or Auto \- in versions with this feature).  
* **Log Viewer:** The /log endpoint (or a section on the main page) displays real-time operational logs from the device.  
* **Status Endpoint (ESP32-C3 v4.0+):** The /status endpoint provides a JSON object with current device and printer state, useful for programmatic access or dynamic UI updates.

### **LED Status Indication (General Guide \- may vary slightly between versions)**

* **AP Mode (WiFi Configuration):**  
  * **ESP8266:** Yellow/Green alternating (if no client connected to AP), solid Green (if client connected to AP). Typically involves the first LED.  
  * **ESP32-C3:** Solid Blue on the first LED (or similar indication).  
* **Connecting to WiFi / Connecting to Printer:** Red/Blue alternating on the first LED(s).  
* **WiFi Connected (Awaiting Printer Connection):** Solid Blue on the first LED, second LED may blink Red.  
* **Printer Connected (Standby):**  
  * **Rainbow Marquee Mode:** LEDs cycle through rainbow colors in a marquee fashion.  
  * **Breathing Light Mode:** All LEDs breathe with the configured standbyBreathingColor.  
* **Printing:**  
  * LEDs light up as a progress bar using progressBarColor, representing printPercent.  
  * If overlayMarquee is enabled, a rainbow marquee effect is blended over the progress bar.  
* **Error (Print Failed / MQTT Timeout):** LEDs blink Red.  
* **LED Test:** LEDs light up one by one in White.

### **Troubleshooting**

* **No LED activity:** Check power supply to MCU and LED strip. Verify LED\_PIN and LED\_COUNT in the firmware. Ensure the LED data line is securely connected.  
* **Cannot connect to WiFiManager AP:** Ensure no other devices are trying to configure simultaneously. Try moving closer to the device.  
* **Device not connecting to WiFi:** Double-check WiFi SSID and password. Ensure your WiFi is 2.4GHz (ESP8266 requirement).  
* **No printer status / LEDs not updating correctly:**  
  * Verify UID, Access Token, and Device Serial Number are correct in the web configuration.  
  * Check MQTT server (cn.mqtt.bambulab.com) and port (8883) are correct.  
  * Ensure the device has internet access and can reach the MQTT broker.  
  * Check the /log page for error messages.  
  * The printer itself must be online and connected to Bambu Lab cloud services.  
* **Web interface not loading:** Check the device's IP address. Ensure it's connected to your network. Try clearing browser cache.  
* **Device unstable / restarting:**  
  * Ensure adequate power supply, especially if using many LEDs.  
  * Check for low heap memory warnings in the serial log or /log page. Later versions have better memory management.  
  * Update to the latest stable firmware version for your MCU.

## **中文 (Chinese)**

该项目为 ESP8266 和 ESP32-C3 微控制器提供固件，旨在使用 NeoPixel LED 灯带显示 Bambu Lab 3D 打印机的状态。它会连接到您的 WiFi 网络，通过 MQTT 与打印机通信，并提供一个网页界面用于配置。

### **功能特性**

* **打印机状态指示:** 在 LED 灯带上显示打印机状态（例如，打印进度、待机、错误）。  
* **WiFi 配置:** 使用 WiFiManager (ESP8266) 或标准 WiFi 设置 (ESP32-C3) 通过强制门户轻松配置网络。  
* **MQTT 通信:** 连接到 Bambu Lab MQTT代理以接收实时打印机状态。  
* **网页界面:** 提供一个网页用于：  
  * 查看当前打印机和设备状态。  
  * 配置 WiFi（初始设置）。  
  * 配置 MQTT 凭据（UID、访问令牌、设备序列号）。  
  * 自定义 LED 行为（亮度、待机模式、颜色、叠加效果）。  
  * 测试 LED 灯带。  
  * 重置配置或重启设备。  
* **持久化配置:** 将设置保存到微控制器上的 LittleFS 文件系统中。  
* **可定制的 LED 效果:**  
  * 打印进度条。  
  * 待机模式：彩虹跑马灯、呼吸灯。  
  * 可为不同状态配置颜色和亮度比例。  
  * 打印时可选的跑马灯叠加效果。  
* **看门狗定时器:** 为提高稳定性而实现（尤其是在较新的 ESP8266 版本和 ESP32-C3 中）。  
* **内存优化:** 较新版本利用 PROGMEM 存储静态内容以节省 RAM。  
* **MQTT 缓冲 (ESP8266 v3.4+ / ESP32-C3):** 将 MQTT 消息缓冲到文件，以更优雅地处理断开连接或资源限制的情况。

### **硬件要求**

* **微控制器:**  
  * ESP8266 (例如 Wemos D1 Mini, NodeMCU)  
  * ESP32-C3 (例如 ESP32-C3-DevKitM-1, 或像 ESP32-C3 代码中提到的 Mini-1-H4 这样的定制板)  
* **LED 灯带:** Adafruit NeoPixel 兼容灯带 (WS2812B, WS2811 等)。默认配置为 20 个 LED。  
* **电源:** 适用于 MCU 和 LED 灯带的电源。  
* **接线:** MCU、LED 灯带和电源的标准连接。

### **软件要求**

* **开发环境:** PlatformIO IDE (推荐) 或 Arduino IDE。  
* **库 (由 PlatformIO 管理或在 Arduino IDE 中手动安装):**  
  * **通用库:**  
    * Adafruit NeoPixel  
    * ArduinoJson (推荐 v6.x)  
    * PubSubClient (by knolleary)  
    * LittleFS  
    * WiFiClientSecure  
  * **ESP8266 特定库:**  
    * ESP8266WiFi  
    * WiFiManager (by tzapu)  
    * ESP8266WebServer  
    * Ticker (用于某些版本中的看门狗)  
  * **ESP32-C3 特定库 (参考 ESP32-C3 的 platformio.ini):**  
    * WiFi (ESP32 核心库)  
    * WiFiManager (by tzapu, ESP32 也可使用其他 WiFi 配置方式)  
    * ESPAsyncWebServer (by me-no-dev)  
    * esp\_task\_wdt (ESP32 核心库，用于看门狗)

### **固件版本**

#### **ESP8266 系列**

* **v1.8 & v2.0:**  
  * 基础功能：通过 WiFiManager 连接 WiFi，连接到 Bambu Lab 的 MQTT，基本的 LED 状态显示（进度条、简单待机模式），以及用于配置的网页界面。  
  * 配置包括 UID、访问令牌、设备ID、全局亮度、待机模式（跑马灯/呼吸灯）、进度条颜色、待机呼吸灯颜色、亮度比例和跑马灯叠加。  
  * LED 测试功能。  
  * 单设备 Web 访问控制（一次只允许一个客户端配置）。  
  * 基础看门狗定时器。  
  * *注意：根据提供的代码，v1.8 和 v2.0 在功能上似乎是相同的。*  
* **v2.2:**  
  * **本地化:** 引入了中文注释和网页界面文本。  
  * **增强型看门狗:** 使用 Ticker 实现更具体的看门狗，用于监控 LED 渲染、模式判断和低堆内存。  
  * **可配置的 Pushall 间隔:** 添加了 customPushallInterval 用于 MQTT pushall 请求，可通过网页界面配置并保存到 config.json。  
  * **NTP 同步超时:** 为 NTP 时间同步添加了5秒超时。  
  * **改进的 LED 效果:**  
    * 进度条的部分像素渲染更精确。  
    * 跑马灯叠加效果现在与进度条颜色混合。  
  * **网页界面反馈:** 改为使用 JavaScript alert() 进行配置保存/测试反馈和客户端重定向。  
  * **强制模式:** 引入了“强制模式”（进度条/待机），可通过网页界面端点控制，允许手动覆盖 LED 显示状态以进行测试或满足偏好。  
  * **详细日志:** 增强了 /log 端点以提供更全面的状态信息。  
* **v3.0:**  
  * **Web 服务优化:**  
    * 重大更改：引入从 LittleFS 提供静态 /index.html 文件，而不是在内存中动态生成 HTML 字符串。这显著减少了 Web 请求期间的 RAM 使用。  
    * 添加了 isWebServing 和 pauseLedUpdate 标志，以在网页传输期间管理系统资源并防止 LED/MQTT 干扰。  
    * 实现了 sendFileInChunks 以分块发送静态 HTML。  
  * **NTP 服务器更改:** NTP 服务器切换到 ntp.aliyun.com。  
  * **改进的 Web 表单:** 更强大的客户端 JavaScript 表单输入验证（例如，颜色十六进制格式）。  
  * **延迟的 Pushall:** 如果正在提供网页，则 MQTT pushall 请求将被延迟（pendingPushall 标志）。  
* **v3.2:**  
  * **MQTT 稳定性:**  
    * 引入了 MQTT 响应超时 (mqttResponseTimeout)。如果在此期间未收到 MQTT 消息，则打印机将被标记为 printerOffline。  
    * 如果打印机标记为离线，系统会尝试重新连接到 MQTT。  
  * **打印机离线指示:** printerOffline 状态会反映在网页界面和日志中。  
  * **MQTT 数据验证:** 在 mqttCallback 中添加了对 null 或缺失 "print" 数据的检查，以更好地检测离线打印机或无效消息。  
* **v3.4:**  
  * **PROGMEM 优化:** 广泛使用 PROGMEM 存储常量字符串（MQTT 服务器/主题、HTML 内容），以显著减少 RAM占用。  
  * **MQTT 消息文件缓冲:**  
    * 为接收 (RX) 和待发送 (TX) 的消息实现了到 LittleFS 的 MQTT 消息缓冲。  
    * writeMqttRxBuffer 和 processMqttRxBuffer: 传入消息被写入 MQTT\_RX\_BUFFER\_FILE 并从中处理，使系统能够处理大消息或在资源可用时处理它们。  
    * writeMqttTxBuffer 和 processMqttTxBuffer: 传出消息（如 pushall）被写入 MQTT\_TX\_BUFFER\_FILE 并在 MQTT 客户端就绪时发送。  
    * 这有助于在低内存情况下提高可靠性。  
  * **MQTT 重连退避:** 为 MQTT 重连尝试实现了指数退避（reconnectDelay 最长可达 maxReconnectDelay）。  
  * **增强的 Web 服务:** sendFileInChunks 进一步优化，在堆内存极低时直接发送 PROGMEM HTML。在 Web 服务期间引入 pauseMqttUpdate。  
  * **看门狗优化:** 看门狗超时增加；在 Web 服务期间跳过低堆内存检查。  
  * **视觉更改:** 跑马灯流动方向反转（从右到左）。  
  * **网页界面增强:** 添加了“重启设备”按钮。对配置参数进行了更严格的服务器端验证。

#### **ESP32-C3 系列**

ESP32-C3 版本基于 ESP8266 后期版本的稳定性和功能，并针对 ESP32 平台进行了代码调整。

* **与 ESP8266 的平台差异:**  
  * 使用 WiFi.h (ESP32 核心库)。  
  * 使用 ESPAsyncWebServer 实现非阻塞 Web 服务器操作，更适合 ESP32 的能力。  
  * 利用 ESP32 内置的任务看门狗 (esp\_task\_wdt)。  
  * 由于 ESP32 上有更多可用 RAM，因此使用容量更大的 DynamicJsonDocument。  
  * MQTT 的客户端 ID 使用 ESP.getEfuseMac() 生成。  
* **v4.0:**  
  * **核心逻辑移植:** 从 ESP8266 v3.4 移植了大部分功能，包括使用 PROGMEM 存储 HTML、基于文件的 MQTT 缓冲、可配置参数和 LED 效果。  
  * **LED 引脚:** 为 ESP32-C3 Mini-1-H4 上的 GPIO8 配置。  
  * **网页界面:** 调整为使用 ESPAsyncWebServer 的语法和参数处理。  
  * **NTP 服务器:** 使用 pool.ntp.org 和 time.windows.com。  
  * **MQTT 超时:** 包含 MQTT 消息超时；如果未收到消息，currentState 将设置为 ERROR。  
  * **堆内存管理:** 监控堆内存，如果低于 20,000 字节则重启。  
  * **彩虹颜色算法:** 与最终的 ESP8266 版本相比，getRainbowColor 使用了略有不同的算法，基于直接的色相映射。  
  * **跑马灯方向:** 从左向右流动。  
* **v4.2:**  
  * **跑马灯方向:** 跑马灯流动方向更改为从右到左（与 ESP8266 v3.4 相同）。  
  * **打印完成行为:** 当打印完成（gcodeState \== "FINISH" 或 printPercent \>= 99）时，任何 forcedMode 都会自动重置为 NONE。这通过在强制状态后返回自动状态显示来提高可用性。  
  * **网页界面颜色输入:** JavaScript 中对同步 \<input type='color'\> 和文本十六进制输入的细微调整，确保 \# 前缀处理一致。  
  * **次要修复:** 可能的次要稳定性改进和日志记录调整。

### **安装与配置**

1. **烧录固件:**  
   * 使用 PlatformIO 构建并将所需的固件版本上传到您的 ESP8266 或 ESP32-C3 开发板。  
   * 确保在 platformio.ini 中为您的开发板选择了正确的环境（尤其是对于 ESP32-C3）。  
2. **初始 WiFi 配置:**  
   * 首次启动或 WiFi 凭据丢失时，设备将以接入点 (AP) 模式启动。  
   * AP SSID 通常为 "BambuAP" (ESP8266) 或 "BambuLED-Setup" (ESP32-C3)。  
   * 使用手机或电脑连接到此 AP。应出现强制门户页面（或导航到 192.168.4.1）。  
   * 选择您的 WiFi 网络 (SSID)，输入密码。  
   * \*\*对于在 WiFiManager 中包含 MQTT 设置的版本（例如，ESP32-C3 v4.0+ 如果配置为空）：\*\*输入您的 Bambu Lab UID、访问令牌和设备序列号。  
   * 保存配置。设备将尝试连接到您的 WiFi。  
3. **网页界面配置:**  
   * 连接到您的 WiFi 后，找到设备的 IP 地址（例如，从路由器的客户端列表或串行监视器中）。  
   * 在 Web 浏览器中打开此 IP 地址。  
   * 您可以配置：  
     * MQTT 凭据（如果在 WiFiManager 设置期间未设置）。  
     * 各种状态下的 LED 亮度和颜色设置。  
     * 待机模式和跑马灯效果。  
     * MQTT pushall 间隔。  
   * 保存配置。更改通常会立即应用或在 pushall 请求后应用。

### **网页界面**

网页界面 (/) 允许您：

* 查看当前设备和打印机状态。  
* 修改并保存所有可配置参数：  
  * UID、访问令牌、设备序列号。  
  * 全局 LED 亮度。  
  * 待机模式（彩虹跑马灯、呼吸灯）。  
  * 进度条颜色。  
  * 待机呼吸灯颜色。  
  * 进度条和待机模式的亮度比例。  
  * MQTT pushall 请求间隔。  
  * 在进度条上叠加跑马灯（复选框）。  
* **操作:**  
  * 测试 LED 灯带。  
  * 清除缓存（在较新版本中清除 MQTT 缓冲文件）。  
  * 重置配置（删除 config.json 并重启）。  
  * 重启设备 (ESP8266 v3.4+, ESP32-C3)。  
  * 切换模式（强制进度条、强制待机或自动 \- 在具有此功能的版本中）。  
* **日志查看器:** /log 端点（或主页上的一个部分）显示来自设备的实时操作日志。  
* **状态端点 (ESP32-C3 v4.0+):** /status 端点提供一个包含当前设备和打印机状态的 JSON 对象，可用于程序化访问或动态 UI 更新。

### **LED 状态指示 (通用指南 \- 不同版本可能略有差异)**

* **AP 模式 (WiFi 配置):**  
  * **ESP8266:** 黄/绿交替（如果无客户端连接到 AP），纯绿（如果客户端连接到 AP）。通常涉及第一个 LED。  
  * **ESP32-C3:** 第一个 LED 纯蓝（或类似指示）。  
* **正在连接 WiFi / 正在连接打印机:** 第一个（些）LED 红/蓝交替。  
* **WiFi 已连接 (等待打印机连接):** 第一个 LED 纯蓝，第二个 LED 可能闪烁红色。  
* **打印机已连接 (待机):**  
  * **彩虹跑马灯模式:** LED 以跑马灯方式循环显示彩虹色。  
  * **呼吸灯模式:** 所有 LED 以配置的 standbyBreathingColor 进行呼吸。  
* **打印中:**  
  * LED 以 progressBarColor 作为进度条点亮，代表 printPercent。  
  * 如果启用了 overlayMarquee，则彩虹跑马灯效果会叠加在进度条上。  
* **错误 (打印失败 / MQTT 超时):** LED 闪烁红色。  
* **LED 测试:** LED 逐个以白色点亮。

### **故障排除**

* **LED 无活动:** 检查 MCU 和 LED 灯带的电源。验证固件中的 LED\_PIN 和 LED\_COUNT。确保 LED 数据线连接牢固。  
* **无法连接到 WiFiManager AP:** 确保没有其他设备同时尝试配置。尝试靠近设备。  
* **设备无法连接到 WiFi:** 仔细检查 WiFi SSID 和密码。确保您的 WiFi 是 2.4GHz（ESP8266 要求）。  
* **无打印机状态 / LED 未正确更新:**  
  * 验证 Web 配置中的 UID、访问令牌和设备序列号是否正确。  
  * 检查 MQTT 服务器 (cn.mqtt.bambulab.com) 和端口 (8883) 是否正确。  
  * 确保设备具有互联网访问权限并且可以访问 MQTT 代理。  
  * 检查 /log 页面是否有错误消息。  
  * 打印机本身必须在线并连接到 Bambu Lab 云服务。  
* **网页界面无法加载:** 检查设备的 IP 地址。确保它已连接到您的网络。尝试清除浏览器缓存。  
* **设备不稳定 / 重启:**  
  * 确保电源充足，尤其是在使用大量 LED 时。  
  * 在串行日志或 /log 页面中检查低堆内存警告。较新版本具有更好的内存管理。  
  * 为您的 MCU 更新到最新的稳定固件版本。