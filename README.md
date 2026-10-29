# Smart Aqua Pump: Monitoring and Controlling System

[![Arduino](https://img.shields.io/badge/Arduino-IDE-00979D?style=for-the-badge&logo=arduino&logoColor=white)](https://www.arduino.cc/)
[![ESP32](https://img.shields.io/badge/ESP32-Microcontroller-E7352C?style=for-the-badge&logo=espressif&logoColor=white)](https://www.espressif.com/)
[![EMQX Cloud](https://img.shields.io/badge/EMQX-Cloud%20MQTT-00B4D8?style=for-the-badge&logo=emqx&logoColor=white)](https://www.emqx.com/en/cloud)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

An IoT-based intelligent water pump monitoring and controlling system powered by **ESP32**, **Arduino IDE**, and **EMQX Cloud**. The system features real-time monitoring, automated water level control, dry-run protection, and robust **online & offline scheduling** capabilities to keep your pump running even when internet connectivity is lost.

---

## 📽️ Demo & Tutorial Video

Watch the complete project overview and step-by-step setup on YouTube:

[![Smart Aqua Pump Video](https://img.youtube.com/vi/hEJ8uvH4t4Y/hqdefault.jpg)](https://www.youtube.com/watch?v=hEJ8uvH4t4Y)

👉 **[Click Here to Watch the Full Video Demonstration](https://www.youtube.com/watch?v=hEJ8uvH4t4Y)**

---

## ✨ Key Features

* **🌐 Dual Operating Modes (Online & Offline):**
  * **Online Mode:** Control and monitor the pump from anywhere using MQTT protocol via **EMQX Cloud**.
  * **Offline Mode:** Local scheduling and automated sensor logic continue running seamless operation even if Wi-Fi or cloud connection drops.
* **⏰ Smart Scheduling:** Set daily start and stop times for automatic water pump cycles (stored locally and synced with cloud servers).
* **🔄 Automated Pump Control:** Automatic trigger (ON/OFF) based on upper and lower tank threshold levels.
* **🛡️ Dry-Run & Overflow Protection:** Automatically shuts off the motor when no water supply is detected to prevent pump damage.
* **📊 Real-time Data Telemetry:** Stream water level percentage, pump status, and device diagnostics directly to your mobile app/web dashboard via **EMQX Cloud**.

---

## 🛠️ Hardware & Tech Stack

### Microcontrollers & Connectivity
* **ESP32** / **ESP8266** Microcontroller board
* **EMQX Cloud** (Managed MQTT Broker)
* **Arduino IDE** (Programming environment)

### Components & Sensors
* **Ultrasonic Sensor (HC-SR04)** or Water Level Sensor module
* **5V Relay Module** (Controls high-voltage AC/DC pump motor)
* **16x2 LCD with I2C** or OLED Display (Local real-time interface)
* **Real-Time Clock (RTC) module / Internal NTP Sync** (For precise offline scheduling)
* **LEDs & Buzzer** (Local alarm and status indication)

---

## 📐 System Architecture

```
                                  +-----------------------+
                                  |   EMQX Cloud Broker   |
                                  |   (MQTT Server)       |
                                  +-----------^-----------+
                                              |
                                     Wi-Fi / MQTT Sync
                                              |
 +------------------+            +------------v----------+            +-------------------+
 | Ultrasonic /     | ---------> |  ESP32 Microcontroller| ---------> | 5V Relay Module   |
 | Level Sensors    |  (Inputs)  |  (Arduino Framework)  |  (Output)  | (Water Pump Control)|
 +------------------+            +------------+----------+            +-------------------+
                                              |
                                              v
                                 +------------------------+
                                 |  Offline EEPROM Sync / |
                                 |  Local LCD Indicator   |
                                 +------------------------+
```

---

## 🚀 Quick Start Guide

### 1. Prerequisites
* Install [Arduino IDE](https://www.arduino.cc/en/software).
* Add ESP32 support to Arduino IDE via **Boards Manager**.
* Install the following required libraries:
  * `PubSubClient` (for EMQX MQTT connectivity)
  * `WiFiManager` or `WiFi.h`
  * `ArduinoJson`
  * `LiquidCrystal_I2C`

### 2. EMQX Cloud Setup
1. Create a free instance on [EMQX Cloud](https://www.emqx.com/en/cloud).
2. Create an MQTT client credential (Username & Password).
3. Copy your Cloud **Broker Address** and **Port** (e.g., `xxxxxx.emqx.cloud:8883` or `1883`).

### 3. Firmware Installation
1. Clone this repository:
   ```bash
   git clone https://github.com/rkrazowan/Smart-Aqua-pump-Monitoring-and-controlling-Project.git
   ```
2. Open the main `.ino` file in **Arduino IDE**.
3. Update your credentials in the configuration section:
   ```cpp
   // Wi-Fi Credentials
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";

   // EMQX Cloud MQTT Credentials
   const char* mqtt_broker = "YOUR_EMQX_BROKER_ADDRESS";
   const char* mqtt_user   = "YOUR_MQTT_USERNAME";
   const char* mqtt_pass   = "YOUR_MQTT_PASSWORD";
   const int   mqtt_port   = 1883;
   ```
4. Upload the code to your **ESP32** board.

---

## ⚙️ Scheduling Logic

```
   +--------------------------------------------------------+
   |                  System Power ON                       |
   +---------------------------+----------------------------+
                               |
                   +-----------v-----------+
                   | Check Network Status  |
                   +-----+-----------+-----+
                         |           |
            Connected    |           | Offline
         +---------------+           +---------------+
         |                                           |
+--------v------------------+               +--------v------------------+
| Online Mode:              |               | Offline Mode:             |
| - Sync time via NTP       |               | - Use saved local timer   |
| - Receive MQTT schedules  |               | - Execute EEPROM schedule |
| - Stream live telemetry   |               | - Local sensor triggers   |
+---------------------------+               +---------------------------+
```

---

## 🤝 Contributing

Contributions, bug reports, and feature requests are welcome!  
Feel free to open an issue or submit a pull request on the [GitHub Issues](https://github.com/rkrazowan/Smart-Aqua-pump-Monitoring-and-controlling-Project/issues) page.

---

## 📜 License

This project is open-source and licensed under the [MIT License](LICENSE).
