# Ninebot G30 BLE Telemetry

BLE telemetry, authentication and dashboard experiments for the **Ninebot MAX G30** using the legacy NinebotCrypto session protocol.

> **Frozen protocol baseline:** `v6.1` - hardware-tested read-only telemetry  
> **DPC test baseline:** `v6.2 DPC ON` - authenticated `0x76 = 0x0001` write after READY  
> **Current CYD dashboard:** `v7.14 Self-Bound AutoAuth` - 240x320 portrait, AutoConnect, fast telemetry, DPC verify and persistent self-bound authentication

The older v6.x files remain unchanged as protocol/reference freezes. The current CYD build lives in `CYD_v7_14_SelfBoundAutoAuth/`.

## Current v7.14 highlights

- Classic ESP32-2432S028R / "Cheap Yellow Display" (ILI9341, 240x320 portrait)
- Ninebot legacy BLE / Nordic UART Service
- callback-safe NinebotCrypto transport; BLE writes stay outside notify callbacks
- target-locked AutoConnect to the previously authenticated scooter
- optimized LVGL dirty-region renderer
- 100 ms telemetry scheduler with speed/current prioritized
- DPC register `0x76 = 0x0001` after READY, with read-back verification
- self-bound credential persistence introduced in v7.14
- optional explicit Segway app key (`<SERIAL>_decrypt`) support remains available

## BLE service

| Function | UUID |
|---|---|
| Nordic UART Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| Write / RX characteristic | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| Notify / TX characteristic | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |
| CCCD | `0x2902` |

## Pairing and AutoAuth

### First pairing

With no stored credential, v7.14 uses the normal G30 pairing sequence:

```text
BLE connect
  -> enable notifications
  -> 0x5B INIT
  -> receive BLE key + scooter serial
  -> generate fresh 16-byte dashboard key
  -> 0x5C PING with that key
  -> 0x5C00 while confirmation is pending
  -> press scooter power button once
  -> 0x5C01
  -> session key = SHA1(dashboard_key || BLE_key)
  -> 0x5D PAIR with scooter serial
  -> 0x5D01
  -> READY
```

The dashboard key is persisted **only after a successful `0x5D01`** and is stored together with the exact scooter serial. A failed or incomplete pairing is never promoted to a valid credential.

### Later boots: v7.14 Self-Bound AutoAuth

If an accepted dashboard key exists for the exact scooter serial, v7.14 first tries the already-bound-client path:

```text
0x5B INIT
  -> load stored dashboard key for exact serial
  -> session key = SHA1(stored_key || BLE_key)
  -> direct 0x5D PAIR with scooter serial
  -> 0x5D01
  -> READY
```

No `0x5C` is sent in this path, so no new power-button confirmation is requested.

If the stored credential is rejected or direct `0x5D` times out, v7.14 clears that credential, reconnects cleanly and falls back to one normal fresh-key button pairing. It does not keep retrying a rejected credential forever.

> **Validation note:** the normal G30 `0x5B -> 0x5C -> power button -> 0x5D01` path is hardware-tested. The v7.14 self-bound direct reconnect is the current AutoAuth path and can depend on the BLE/dashboard firmware accepting that stored client binding.

Full details: [`docs/PAIRING.md`](docs/PAIRING.md)

## Optional official Segway app key

A real 16-byte Segway app key (`<SERIAL>_decrypt`) can be provisioned over Serial and takes priority over the self-bound dashboard credential:

```text
APPKEY?                  show key status
APPKEY=<32 hex chars>    store 16-byte Segway app key
APPKEY=CLEAR             clear it
```

Do not confuse this with the temporary `APP random/key` printed during a fresh button pairing.

## AutoConnect target lock

After the first successful authentication, the dashboard stores the scooter name and only reconnects to that target on later scans.

```text
TARGET?       show stored target
TARGET=CLEAR clear target binding
```

If the scooter is powered off, the dashboard simply keeps scanning. Empty scan cycles are normal and do not trigger a reboot.

## DPC

After authentication, the current dashboard build writes ESC register `0x76 = 0x0001`, waits briefly, then reads `0x76` back. A mismatch/timeout is retried with a bounded retry count. A slow periodic check keeps DPC enabled without polluting the fast telemetry loop.

The logical write payload before NinebotCrypto is:

```text
5A A5 02 3E 20 03 76 01 00
```

The legacy raw CRC bytes (`25 FF`) are not appended manually inside the active encrypted session.

## Telemetry registers

| Device | Register | Value |
|---|---:|---|
| BMS `0x22` | `0x32` | Battery % |
| BMS `0x22` | `0x33` | Battery current |
| BMS `0x22` | `0x34` | Battery voltage |
| BMS `0x22` | `0x35` | Battery temperatures |
| ESC `0x20` | `0x26` | Speed |
| ESC `0x20` | `0x29` + `0x2A` | Odometer |

## Repository layout

- `Ninebot_G30_BLE_Telemetry.ino` - frozen v6.1 read-only protocol baseline
- `Ninebot_G30_BLE_Telemetry_v6_2_DPC_ON.ino` - v6.2 DPC test baseline
- `CYD_v7_14_SelfBoundAutoAuth/` - current classic-ESP32 CYD dashboard build
- `docs/PAIRING.md` - pairing, credential persistence and AutoAuth details
- `docs/PROTOCOL.md` / `docs/TESTED.md` - older protocol/reference notes

## CYD build notes

Target: common **ESP32-2432S028R** with ILI9341 240x320 TFT.

The project includes `CYD_TFT_eSPI_User_Setup.h`. TFT_eSPI compiles its own setup separately, so copy/select the equivalent setup in TFT_eSPI before building. Use a classic ESP32 target such as **ESP32 Dev Module** and a large application partition if needed. PSRAM is not required.

## Dependencies

- Arduino ESP32 core BLE stack (`BLEDevice.h`, `BLEUtils.h`, `BLEScan.h`, ...)
- mbedTLS AES/SHA1 headers provided by the ESP32 core
- LVGL 9.x
- TFT_eSPI

No separate NinebotCrypto library is required; the legacy crypto/session handling is implemented in the project source.

## References

- ScooterHacking NinebotCrypto: https://github.com/scooterhacking/NinebotCrypto
- ninebot-ble: https://github.com/ownbee/ninebot-ble
- ha-ninebot: https://github.com/BobMcGlobus/ha-ninebot

## License

No license is included yet. Until you choose one, normal copyright rules apply and GitHub hosting alone does not grant reuse rights.
