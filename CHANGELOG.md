# Changelog

## v7.14 - CYD Self-Bound AutoAuth

Current 240x320 portrait dashboard build for the classic ESP32 CYD.

### Added

- target-locked automatic reconnect to the previously authenticated scooter
- self-bound dashboard credential persisted only after successful `0x5D01`
- later-boot direct `0x5D` AutoAuth attempt using `SHA1(stored_key || BLE_key)`
- clean fallback to one fresh normal button pairing when a stored credential is rejected
- optional explicit Segway app key support (`APPKEY=<32hex>`)
- DPC `0x76 = 0x0001` write with read-back verification and bounded retry
- 100 ms telemetry scheduler with speed/current prioritized
- optimized LVGL dirty-region rendering for 240x320 portrait CYD

### Pairing behavior

First pairing remains the known-good G30 path:

`0x5B -> 0x5C -> 0x5C00 -> power button -> 0x5C01 -> 0x5D -> 0x5D01`

After `0x5D01`, v7.14 stores the accepted dashboard key and exact scooter serial. On later boots it first tries direct `0x5D` without `0x5C`, so a newly accepted bound session does not request another power-button confirmation.

See `docs/PAIRING.md` for the full state machine and validation notes.

### Compatibility / validation

- Normal G30 button pairing: hardware-tested.
- DPC `0x76 = 1` plus readback: hardware-tested in the current dashboard test flow.
- Target-locked AutoConnect: hardware-tested in the current dashboard test flow.
- Self-bound direct reconnect: current v7.14 AutoAuth path; acceptance can depend on BLE/dashboard firmware.

## v6.2 - DPC ON test build

Based on the frozen v6.1 callback-safe auth/telemetry implementation.

### Added

- authenticated ESC write command `0x03`
- DPC register `0x76`
- one-shot DPC ON payload `01 00`
- logical plaintext frame:
  - `5A A5 02 3E 20 03 76 01 00`
- deferred transmission from `loop()` after `0x5D01 / AUTH_READY`
- telemetry is held until the one-shot DPC write has been sent

### Notes

- v6.1 remains unchanged as the hardware-tested freeze baseline.
- v6.2 is a test build until the DPC write is confirmed on hardware.
- The legacy checksum `25 FF` is not manually appended because NinebotCrypto generates the encrypted trailer.

## v6.1 - Frozen working baseline

Hardware-tested successful Ninebot MAX G30 session on ESP32-C3.

### Working

- Nordic UART BLE connection
- notification subscription
- NinebotCrypto `0x5B` initialization
- BLE random/key and serial extraction
- `0x5C` authentication
- physical power-button confirmation
- crypto key transition after `0x5C01`
- `0x5D` pairing/auth confirmation
- callback-safe deferred GATT writes
- fragmented encrypted TX/RX
- read-only telemetry:
  - battery percentage
  - voltage
  - current
  - temperatures
  - speed
  - odometer

### Important fix

BLE writes are deferred out of the notification callback. Earlier versions could stall after the first fragment because a blocking GATT write was initiated from BLE callback context.

### Notes

- No tuning parameters are changed.
- No speed limit is written.
- This release is the project freeze baseline.
