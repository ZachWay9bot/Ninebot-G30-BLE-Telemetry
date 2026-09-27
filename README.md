# Ninebot G30 BLE Telemetry

Read-only BLE telemetry reader for the **Ninebot MAX G30** using an **ESP32-C3** and the legacy NinebotCrypto session protocol.

> **Freeze release:** `v6.1`  
> **Status:** Hardware-tested and working  
> **Freeze SHA-256:** `1f8b6e0c1871de3cb3dbb3483d999f52380e301a436fac98e3e94ce327de7ebd`

## Features

- Connects to the Ninebot dashboard over BLE / Nordic UART Service
- Implements the working NinebotCrypto authentication flow
- Callback-safe BLE writes to avoid GATT deadlocks
- Handles fragmented BLE packets
- Reads telemetry only:
  - battery percentage
  - battery voltage
  - battery current
  - battery temperatures
  - speed
  - odometer
- Does **not** write speed limits, tuning parameters or ESC configuration

## Tested hardware

- ESP32-C3 Dev Module
- Ninebot MAX G30
- Dashboard name observed: `NBScooter2088`
- Serial monitor: `115200 baud`

The code is not tied to that exact advertised name; the tested name is documented so the successful hardware setup remains reproducible.

## BLE service

| Function | UUID |
|---|---|
| Nordic UART Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| Write / RX characteristic | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| Notify / TX characteristic | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |
| CCCD | `0x2902` |

## Authentication flow

The tested scooter uses:

```text
BLE connect
  -> enable notifications
  -> 0x5B INIT
  -> receive BLE random/key + serial number
  -> 0x5C AUTH/PING
  -> 0x5C00 while waiting for physical confirmation
  -> press scooter power button
  -> 0x5C01
  -> switch session key
  -> 0x5D PAIR
  -> 0x5D01
  -> READY
```

A critical implementation detail is that **BLE writes must not be started from inside the notify callback**. The callback only decodes the packet and schedules the next transmission. The actual GATT write is performed later from `loop()`.

## Telemetry registers

| Device | Register | Value |
|---|---:|---|
| BMS `0x22` | `0x32` | Battery % |
| BMS `0x22` | `0x33` | Battery current |
| BMS `0x22` | `0x34` | Battery voltage |
| BMS `0x22` | `0x35` | Battery temperatures |
| ESC `0x20` | `0x26` | Speed |
| ESC `0x20` | `0x29` + `0x2A` | Odometer |

## Arduino IDE

1. Open `Ninebot_G30_BLE_Telemetry.ino`.
2. Select **ESP32C3 Dev Module**.
3. Select the correct COM port.
4. Compile and flash.
5. Open Serial Monitor at **115200 baud**.
6. When the log repeatedly shows `0x5C00`, briefly press the scooter power button.
7. Wait for:

```text
*** AUTHENTIFIZIERUNG ERFOLGREICH - READY ***
```

You should then see output similar to:

```text
>>> AKKU: 100 %
>>> BAT VOLTAGE: 41.37 V
>>> BAT CURRENT: 0.23 A
>>> BAT TEMP: 20 / 20 C
>>> SPEED: 22.2 km/h
>>> ODO: 48.357 km
```

## Dependencies

The sketch uses libraries bundled with the Arduino ESP32 core:

- `BLEDevice.h`
- `BLEUtils.h`
- `BLEScan.h`
- `BLEAdvertisedDevice.h`
- mbedTLS AES/SHA1 headers

No separate NinebotCrypto library is required.

## Freeze policy

`v6.1` is the known-working baseline. Functional changes should be made in a new version rather than silently modifying this release.

The file `FREEZE.sha256` records the SHA-256 of the hardware-tested source. The repository copy is byte-for-byte identical to the frozen sketch except for its filename.

## Known observations

- A `0x5C00` response is expected until the physical power-button confirmation is accepted.
- A single crypto-counter warning was seen during testing, but the session recovered and continued successfully.
- BLE scanning may need more than one scan cycle before the scooter is discovered.
- Pairing/authentication changes BLE authentication state, but this sketch does not modify riding/tuning parameters.

## References

Protocol behavior was cross-checked against community reverse-engineering projects:

- ScooterHacking NinebotCrypto: https://github.com/scooterhacking/NinebotCrypto
- ninebot-ble: https://github.com/ownbee/ninebot-ble
- Community register database: https://github.com/jx-grxf/scooter-tuning-db

## License

No license is included yet. Until you choose one, normal copyright rules apply and GitHub hosting alone does not grant reuse rights.