# G30 Fusion v1 - Arduino IDE Paket

## Getestete Versionen
- Arduino IDE 2.x
- ESP32 by Espressif Systems: 2.0.14
- LVGL: 9.4.0
- TFT_eSPI: 2.5.43
- XPT2046_Touchscreen: 1.4.x

## Installation
1. Arduino IDE öffnen.
2. Boardverwalter: "esp32 by Espressif Systems" Version 2.0.14 installieren.
3. Bibliotheksverwalter:
   - lvgl 9.4.0
   - TFT_eSPI 2.5.43
   - XPT2046_Touchscreen 1.4.x
4. G30_Fusion_Arduino.ino öffnen.
5. Board: "ESP32 Dev Module".
6. Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)" wählen.
7. Richtigen COM-Port wählen.
8. Upload.

Die Dateien build_opt.h und partitions.csv müssen im Sketchordner bleiben.
Sie konfigurieren TFT_eSPI/LVGL und die 3-MB-App-Partition automatisch.

## Hardware
ESP32-2432S028R / CYD:
- TFT MISO 12
- TFT MOSI 13
- TFT SCLK 14
- TFT CS 15
- TFT DC 2
- Backlight 21
- Touch CS 33
- Touch IRQ 36
- Touch SCLK 25
- Touch MISO 39
- Touch MOSI 32

## Erster Start
Beim ersten Start erscheint die 3-Punkt-Touchkalibrierung.
Seriell stehen CAL und UI? als Recovery/Diagnose zur Verfügung.

## Wichtig
Dies ist Firmware für das ESP32-CYD Dashboard, NICHT für den G30-ESC.
Der v7.14 Ninebot BLE/Crypto/AutoAuth/DPC-Backendcode ist unverändert.
