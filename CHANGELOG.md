# Changelog

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