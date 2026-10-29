AQUAPUMP - PERSONAL PACKAGE  (private: contains your broker username and password)
====================================================================================

DO NOT share this package or post it online. Share the "Shareable Package" instead.

Files
-----
AquaPump_ESP32/AquaPump_ESP32.ino   The ESP32 sketch (Version 6). Already contains your broker details.
remote_dashboard.html               Remote dashboard. Already connects to your broker. Open it in Chrome.
AquaPump_Project_Guide.docx         Full step-by-step guide.

Quick start
-----------
1. Arduino IDE: install "esp32 by Espressif Systems" and the library "PubSubClient" (Nick O'Leary).
2. Tools > Board > ESP32 Dev Module. Tools > Partition Scheme > Huge APP (3MB No OTA/1MB SPIFFS).
3. Open AquaPump_ESP32.ino, click Upload (hold the BOOT button if it says Connecting......).
4. Serial Monitor at 115200 should show "Connected. IP: ..." and "MQTT connected".
5. Local dashboard: http://waterpump.local  (or the IP address from the Serial Monitor).
6. Remote dashboard: open remote_dashboard.html in Chrome.

Problems? See Part 14 of the guide.
