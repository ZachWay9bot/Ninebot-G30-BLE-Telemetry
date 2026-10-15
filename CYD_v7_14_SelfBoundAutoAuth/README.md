# ScooterDashFast CYD 240x320 Portrait v7.14

Current classic-ESP32 CYD dashboard build for the Ninebot MAX G30 legacy NinebotCrypto protocol.

## Hardware

Common ESP32-2432S028R "Cheap Yellow Display" with ILI9341 240x320 SPI TFT.

`DISPLAY_ROTATION = 0` is portrait. Use `2` if the physical panel is mounted upside down.

The included `CYD_TFT_eSPI_User_Setup.h` uses the common CYD pins:

- MISO 12
- MOSI 13
- SCLK 14
- CS 15
- DC 2
- RST -1
- backlight 21
- touch CS 33
- 40 MHz TFT SPI

TFT_eSPI compiles its setup separately, so copy/select the equivalent definitions in the TFT_eSPI setup used by your build.

## v7.14

- target-locked AutoConnect
- v7.14 Self-Bound AutoAuth
- optional official Segway app key support
- 100 ms fast telemetry scheduler
- DPC `0x76 = 1` set and verified after READY
- 240x320 portrait LVGL dashboard
- optimized dirty-region redraws
- 0..60 km/h seven-segment speed display
- battery arc, LOAD/REGEN, voltage, power, current, BMS temperature, trip and ODO
- DELTA indicator and status bar

## Pairing

First pairing:

```text
5B -> fresh key -> 5C -> 5C00 -> press power once -> 5C01
   -> session key -> 5D(serial) -> 5D01 -> READY
```

After successful `5D01`, v7.14 stores the accepted dashboard key with the scooter serial.

Later boots first try:

```text
5B -> stored key -> SHA1(key || BLE_key) -> direct 5D(serial) -> 5D01 -> READY
```

No `5C` means no new power-button request. If the stored credential is rejected, it is deleted and the code falls back to one clean normal button pairing.

See `../docs/PAIRING.md` for the full state machine and validation notes.

## Serial commands

```text
APPKEY?                  show explicit Segway app-key status
APPKEY=<32 hex chars>    store a real 16-byte Segway app key
APPKEY=CLEAR             clear explicit app key
TARGET?                  show AutoConnect target
TARGET=CLEAR             clear AutoConnect target
b                        blue theme
r                        red theme
p                        toggle performance logging
```

## Build

Use a classic ESP32 board target matching the CYD, typically `ESP32 Dev Module`. A large APP partition is recommended for the BLE + LVGL build. PSRAM is not required.
