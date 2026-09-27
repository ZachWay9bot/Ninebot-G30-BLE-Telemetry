# Changelog

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