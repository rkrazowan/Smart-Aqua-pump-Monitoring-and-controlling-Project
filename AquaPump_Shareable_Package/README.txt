AQUAPUMP - SHAREABLE PACKAGE
============================

Build your own smart water pump with an ESP32, a relay, a soil sensor and a pump.
This package contains no private passwords. Create your own free EMQX Cloud account (Part 6 of the guide).

Files
-----
AquaPump_Project_Guide.docx         Read this first. Complete step-by-step guide.
AquaPump_ESP32/AquaPump_ESP32.ino   The ESP32 sketch (Version 6).
remote_dashboard.html               Remote dashboard. Asks for your broker details the first time you open it.

Before you upload the sketch, edit these lines near the top of AquaPump_ESP32.ino:

    const char* MQTT_HOST = "YOUR-BROKER-ADDRESS.emqxsl.com";
    const char* MQTT_USER = "YOUR_USERNAME";
    const char* MQTT_PASS = "YOUR_PASSWORD";

Quick start
-----------
1. Arduino IDE: install "esp32 by Espressif Systems" and the library "PubSubClient" (Nick O'Leary).
2. Tools > Board > ESP32 Dev Module. Tools > Partition Scheme > Huge APP (3MB No OTA/1MB SPIFFS).
3. Upload the sketch. Serial Monitor at 115200.
4. Join the Wi-Fi "ESP32_WaterPump" (password 12345678), open http://192.168.4.1,
   Device tab > Home Wi-Fi, and enter your home Wi-Fi.
5. Open http://waterpump.local, or the IP shown in the Serial Monitor.

Safety: use a low-voltage DC pump. Never wire mains voltage unless you are a qualified electrician.
